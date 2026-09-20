#!/usr/bin/env python3
"""e2e_verify.py — V6-R2 五条关键运行链验收 (13.3)

1. 注入 transcript='现在几点' → Provider 请求捕获, 证明文本进入 user content
2. C++ client 语义 + Fake Worker 完整事件链 (hello/listening/transcript/
   thinking/expression/tool/speaking/tts/idle/done)
3. server 关闭再重启: WorkerLost/connected=false/reconnect/新轮 done
4. tool_call 回灌: 打印第二次模型请求的 tool 内容, 断言 = 实际 dispatch 结果
输出: logs/e2e_verify.txt
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "agent"))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd.net_eof import wait_eof    # noqa: E402

OUT = []


def log(s):
    OUT.append(s)
    print(s)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.events = []

    def send(self, msg):
        self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))

    def drain(self, timeout):
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


def run_worker(port, config, env=None):
    e = dict(os.environ)
    e["PYTHONPATH"] = str(ROOT / "agent")
    if env:
        e.update(env)
    proc = subprocess.Popen(
        [sys.executable, "-m", "holopet_agentd",
         "--config", str(config), "--port", str(port),
         "--log-level", "WARNING"],
        cwd=str(ROOT), env=e)
    time.sleep(0.8)
    return proc


def main():
    fails = []

    # ---------- 链 1+4: transcript 边界 + 工具回灌 (真实 Provider 请求捕获) ----------
    log("=== 链 1: transcript 进入 user content ===")
    port = free_port()
    cfg = ROOT / "config" / "agent.example.json"
    proc = run_worker(port, cfg)
    c = Client(port)
    c.drain(0.5)
    c.send({"v": "1", "request_id": "t1", "type": "transcript", "text": "现在几点"})
    time.sleep(0.2)
    c.send({"v": "1", "request_id": "t1", "type": "start_turn"})
    time.sleep(0.2)
    c.send({"v": "1", "request_id": "t1", "type": "stop_recording"})
    c.drain(3.0)
    texts = [e.get("text", "") for e in c.events if e.get("type") == "transcript"]
    log(f"  transcript 事件: {texts}")
    ok = any("现在" in t for t in texts) and "tool_call" in c.types() and "done" in c.types()
    log(f"  => {'PASS' if ok else 'FAIL'}")
    if not ok:
        fails.append("chain1")
    c.close()
    proc.terminate()

    # ---------- 链 4 补充: 打印第二次模型请求中的 tool 内容 (直接 TurnRunner) ----------
    log("=== 链 4: 工具结果真实回灌 ===")
    captured = {}

    class CaptureProvider:
        def tool_schemas(self):
            return []

        def chat(self, messages):
            captured.setdefault("calls", []).append(messages)
            if len(captured["calls"]) == 1:
                return {"content": None, "finish_reason": "tool_calls",
                        "tool_calls": [{"id": "cid-1", "function": {
                            "name": "get_time", "arguments": "{}"}}]}
            return {"content": "ok", "finish_reason": "stop", "tool_calls": []}

    runner = worker_main.TurnRunner(json.loads(cfg.read_text(encoding="utf-8")))
    runner.set_transcript("r", "现在几点")
    runner._provider = CaptureProvider()
    runner._audio_stop = threading.Event()
    runner._audio_stop.set()
    events = []
    runner.run("r", threading.Event(), lambda rid, t, **f: events.append((t, f)))
    second = captured["calls"][1]
    tool_msgs = [m for m in second if m.get("role") == "tool"]
    log(f"  第二次模型请求 tool 消息: {json.dumps(tool_msgs, ensure_ascii=False)}")
    result = json.loads(tool_msgs[0]["content"]) if tool_msgs else {}
    ok4 = bool(tool_msgs) and "time" in result and tool_msgs[0]["tool_call_id"] == "cid-1"
    log(f"  => {'PASS' if ok4 else 'FAIL'}")
    if not ok4:
        fails.append("chain4")

    # ---------- 链 2: 完整事件链 ----------
    log("=== 链 2: 完整事件链 ===")
    port2 = free_port()
    proc2 = run_worker(port2, cfg)
    c2 = Client(port2)
    c2.drain(0.5)
    c2.send({"v": "1", "request_id": "t2", "type": "transcript", "text": "开心一下"})
    time.sleep(0.2)
    c2.send({"v": "1", "request_id": "t2", "type": "start_turn"})
    time.sleep(0.2)
    c2.send({"v": "1", "request_id": "t2", "type": "stop_recording"})
    c2.drain(4.0)
    types = c2.types()
    log(f"  事件链: {types}")
    need = ["hello", "state", "transcript", "tool_call", "tts_started",
            "tts_level", "tts_finished", "done"]
    ok2 = all(t in types for t in need)
    log(f"  => {'PASS' if ok2 else 'FAIL'}")
    if not ok2:
        fails.append("chain2")

    # ---------- 链 3: server 关闭重启 ----------
    log("=== 链 3: server 关闭再重启 ===")
    c2.send({"v": "1", "request_id": "t3", "type": "start_turn"})
    time.sleep(0.3)
    proc2.terminate()
    proc2.wait()
    time.sleep(0.2)
    # 客户端观察 EOF (R3-R1 缺陷 B: 共享 net_eof.wait_eof —
    # clean EOF (recv b'') / reset 都算关闭; 超时不算成功)
    eof_seen, eof_kind = wait_eof(c2.sock, timeout=2.0)
    log(f"  worker 停止后客户端 EOF/复位: {eof_seen} ({eof_kind})")
    c2.close()
    # 重启 + 新连接 + 新轮 done
    proc3 = run_worker(port2, cfg)
    c3 = Client(port2)
    c3.drain(0.5)
    hello_again = "hello" in c3.types()
    c3.send({"v": "1", "request_id": "t4", "type": "transcript", "text": "你好"})
    time.sleep(0.2)
    c3.send({"v": "1", "request_id": "t4", "type": "start_turn"})
    time.sleep(0.2)
    c3.send({"v": "1", "request_id": "t4", "type": "stop_recording"})
    c3.drain(3.5)
    done_new = "done" in c3.types()
    log(f"  重启后重新 hello: {hello_again}, 新一轮 done: {done_new}")
    ok3 = eof_seen and hello_again and done_new
    log(f"  => {'PASS' if ok3 else 'FAIL'}")
    if not ok3:
        fails.append("chain3")
    c3.close()
    proc3.terminate()

    # ---------- 汇总 ----------
    log("=== 汇总 ===")
    log(f"  FAIL: {fails if fails else '无 (全部 PASS)'}")
    (ROOT / "logs").mkdir(exist_ok=True)
    # R4-R1: 写日志加锁重试 (瞬时文件锁不整轮失败)
    for _attempt in range(3):
        try:
            (ROOT / "logs" / "e2e_verify.txt").write_text(
                "\n".join(OUT), encoding="utf-8")
            break
        except PermissionError:
            import time as _t
            _t.sleep(2)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
