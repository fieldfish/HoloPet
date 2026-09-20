"""test_turn_runner.py — Turn 串行/忙拒绝/工具真实回灌/transcript 边界  V6-R2

关键断言:
  - 忙时第二个 start_turn 被结构化拒绝 (busy), 事件不交错
  - transcript 按 request_id 保存并成为 LLM 实际 user content (非固定文本)
  - 工具 dispatch 的实际 JSON 结果作为 tool 消息回灌进下一次模型请求
  - 无 transcript 的 turn → 结构化 stt 错误
  - cancel 幂等 + 完成后清理
"""

import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Client:
    """JSON Lines 测试客户端"""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.events = []

    def send(self, msg: dict):
        self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))

    def drain(self, timeout=1.5):
        self.sock.settimeout(timeout)
        end = time.time() + timeout
        while time.time() < end:
            try:
                d = self.sock.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            self.buf += d
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                self.events.append(json.loads(line.strip()))

    def types(self):
        return [e.get("type") for e in self.events]

    def close(self):
        self.sock.close()


class TestTurnRunner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = _free_port()
        # R5_R4: 禁用持久存储 (固定 rid/call_id 会跨进程撞幂等记录)
        cls.config = {"provider": "fake", "max_tool_rounds": 4,
                      "state_db_path": ""}
        cls.srv = worker_main.WorkerServer(cls.config, port=cls.port)
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()
        time.sleep(0.3)

    def setUp(self):
        self.c = Client(self.port)
        time.sleep(0.1)
        self.c.drain(0.3)      # 收 hello

    def tearDown(self):
        self.c.close()

    def test_transcript_boundary_enters_user_content(self):
        # 注入 transcript='现在几点' → LLM 用该文本 (走 get_time 工具链)
        self.c.send({"v": "1", "request_id": "t1", "type": "transcript",
                     "text": "现在几点"})
        time.sleep(0.2)
        self.c.send({"v": "1", "request_id": "t1", "type": "start_turn"})
        time.sleep(0.2)
        self.c.send({"v": "1", "request_id": "t1", "type": "stop_recording"})
        self.c.drain(3.0)
        types = self.c.types()
        self.assertIn("tool_call", types, f"期望 tool_call, got {types}")
        texts = [e.get("text", "") for e in self.c.events if e.get("type") == "transcript"]
        self.assertTrue(any("现在" in t for t in texts),
                        f"transcript 必须回显注入文本, got {texts}")
        self.assertIn("done", types)

    def test_no_transcript_is_structured_error(self):
        # 无注入 → STT 结构化错误 (不伪造固定文本)
        self.c.send({"v": "1", "request_id": "t2", "type": "start_turn"})
        time.sleep(0.2)
        self.c.send({"v": "1", "request_id": "t2", "type": "stop_recording"})
        self.c.drain(3.0)
        errors = [e.get("text", "") for e in self.c.events if e.get("type") == "error"]
        self.assertTrue(any("stt:no_transcript" in t for t in errors),
                        f"期望 stt:no_transcript 错误, got {errors}")

    def test_busy_rejected(self):
        # 第一个 turn 未结束 (listening 等待) 时第二个 start_turn → busy
        self.c.send({"v": "1", "request_id": "a", "type": "start_turn"})
        time.sleep(0.15)
        self.c.send({"v": "1", "request_id": "b", "type": "start_turn"})
        self.c.drain(0.5)
        busy = [e for e in self.c.events
                if e.get("type") == "error" and e.get("request_id") == "b"]
        self.assertTrue(any("busy" in e.get("text", "") for e in busy),
                        "第二个 turn 应被 busy 拒绝")
        # 清理第一个 turn
        self.c.send({"v": "1", "request_id": "a", "type": "cancel"})
        self.c.drain(0.5)

    def test_cancel_idempotent(self):
        self.c.send({"v": "1", "request_id": "c1", "type": "start_turn"})
        time.sleep(0.15)
        self.c.send({"v": "1", "request_id": "c1", "type": "cancel"})
        self.c.send({"v": "1", "request_id": "c1", "type": "cancel"})   # 幂等
        self.c.drain(1.0)
        types = self.c.types()
        self.assertIn("cancel", types)

    def test_tool_result_roundtrip(self):
        # 直接跑 TurnRunner 无锁路径: 捕获 provider 第二次请求的 messages
        captured = {}
        class FakeHttpProvider:
            def __init__(self, cap):
                self.cap = cap
            def tool_schemas(self):
                return []
            def chat(self, messages):
                self.cap.setdefault("calls", []).append(messages)
                n = len(self.cap["calls"])
                if n == 1:
                    return {"content": None, "finish_reason": "tool_calls",
                            "tool_calls": [{"id": "cid-1", "function": {
                                "name": "get_time", "arguments": "{}"}}]}
                return {"content": "ok", "finish_reason": "stop", "tool_calls": []}

        runner = worker_main.TurnRunner(self.config)
        runner.set_transcript("rr", "现在几点")
        runner._provider = FakeHttpProvider(captured)
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        events = []
        cancel = threading.Event()
        runner.run("rr", cancel,
                   lambda r, t, **f: events.append((t, f)))
        self.assertEqual(len(captured["calls"]), 2, "应有两次模型请求")
        second = captured["calls"][1]
        tool_msgs = [m for m in second if m.get("role") == "tool"]
        self.assertEqual(len(tool_msgs), 1)
        # 第二次请求中的 tool 内容 = dispatch 的实际结果 (非固定 "ok")
        result = json.loads(tool_msgs[0]["content"])
        self.assertIn("time", result, f"回灌必须是实际工具结果, got {result}")
        self.assertEqual(tool_msgs[0]["tool_call_id"], "cid-1")
        self.assertIn(("done", {}), events)

    def test_multi_tool_calls_all_roundtrip(self):
        captured = {}
        class MultiToolProvider:
            def __init__(self, cap):
                self.cap = cap
            def tool_schemas(self):
                return []
            def chat(self, messages):
                self.cap.setdefault("calls", []).append(messages)
                if len(self.cap["calls"]) == 1:
                    return {"content": None, "finish_reason": "tool_calls",
                            "tool_calls": [
                                {"id": "a1", "function": {"name": "get_time",
                                                          "arguments": "{}"}},
                                {"id": "a2", "function": {"name": "set_expression",
                                                          "arguments":
                                                          '{"emotion":"happy"}'}}]}
                return {"content": "done", "finish_reason": "stop", "tool_calls": []}
        runner = worker_main.TurnRunner(self.config)
        runner.set_transcript("mm", "现在几点")
        runner._provider = MultiToolProvider(captured)
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        events = []
        runner.run("mm", threading.Event(),
                   lambda r, t, **f: events.append((t, f)))
        second = captured["calls"][1]
        tool_msgs = [m for m in second if m.get("role") == "tool"]
        self.assertEqual(len(tool_msgs), 2, "两个 tool 结果都回灌")
        ids = {m["tool_call_id"] for m in tool_msgs}
        self.assertEqual(ids, {"a1", "a2"}, "tool_call_id 一一对应")
        results = [json.loads(m["content"]) for m in tool_msgs]
        self.assertTrue(any("time" in r for r in results))
        self.assertTrue(any(r.get("emotion") == "happy" for r in results))

    def test_max_tool_rounds_error(self):
        cfg = dict(self.config); cfg["max_tool_rounds"] = 1
        class LoopProvider:
            def tool_schemas(self):
                return []
            def chat(self, messages):
                return {"content": None, "finish_reason": "tool_calls",
                        "tool_calls": [{"id": "x", "function": {
                            "name": "get_time", "arguments": "{}"}}]}
        runner = worker_main.TurnRunner(cfg)
        runner.set_transcript("loop", "现在几点")
        runner._provider = LoopProvider()
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        events = []
        runner.run("loop", threading.Event(),
                   lambda r, t, **f: events.append((t, f)))
        errs = [f for t, f in events if t == "error"]
        self.assertTrue(any(f.get("code") in ("tool_rounds_exceeded",
                                                "agent_budget_exceeded")
                               for f in errs), f"{events}")


if __name__ == "__main__":
    unittest.main()
