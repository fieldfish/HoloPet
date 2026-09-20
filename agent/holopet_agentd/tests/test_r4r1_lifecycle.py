"""test_r4r1_lifecycle.py — R4-R1 P3: 隔离/上限/取消终态/有界退出/Provider 边界

覆盖以下生命周期场景:
  - test_connection_transcript_isolation: transcript 按连接绑定
  - test_transcript_limits_and_disconnect_cleanup: 条目/单条/总字节上限,
    断连清理该连接 transcript
  - test_blocking_provider_shutdown_deadline: 阻塞 Provider 下 shutdown
    兑现 deadline 并如实报告非 graceful
  - test_idle_client_closed_on_shutdown: 空闲客户端被 shutdown 主动关闭
  - test_main_finally_calls_shutdown: main 的 finally 调用 shutdown
  - test_local_http_provider_timeout_cancel_size_limit: OpenAI Compatible
    仅用本地 HTTP mock 测请求格式/大小上限/超时 (无真实网络)
"""

import http.server
import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd.providers.openai_compat import (  # noqa: E402
    OpenAICompatibleProvider)

CONFIG = {"provider": "fake", "max_tool_rounds": 4}


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class _MockHttp(http.server.BaseHTTPRequestHandler):
    payload_size = 200
    delay_s = 0.0
    status = 200
    captured = {}

    def do_POST(self):
        self.__class__.captured["path"] = self.path
        n = int(self.headers.get("Content-Length", 0))
        self.__class__.captured["body"] = self.rfile.read(n).decode("utf-8")
        self.__class__.captured["auth"] = self.headers.get("Authorization", "")
        if self.delay_s:
            time.sleep(self.delay_s)
        body = json.dumps(
            {"choices": [{"message": {"content": "x" * self.payload_size},
                          "finish_reason": "stop"}]}).encode("utf-8")
        self.send_response(self.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class _QuietHTTPServer(http.server.ThreadingHTTPServer):
    """R7_R1 (C2, 用户批准窄范围): 只静默客户端主动断连三类异常,
    不吞其他异常 (其余异常仍走默认 handle_error 打印, 保证可见)。"""

    def handle_error(self, request, client_address):
        import sys as _sys
        exc = _sys.exception()
        if isinstance(exc, (ConnectionAbortedError, ConnectionResetError,
                            BrokenPipeError)):
            return   # 测试主动断连属预期行为; 不打印 stdlib traceback
        super().handle_error(request, client_address)


class TestR4R1Lifecycle(unittest.TestCase):
    # ---- 1. transcript 连接隔离 ----
    def test_connection_transcript_isolation(self):
        srv = worker_main.WorkerServer(CONFIG, port=0)
        a = worker_main.ClientConn(None)
        b = worker_main.ClientConn(None)
        ok = srv._set_transcript(a, "rid-x", "连接A的文本")
        self.assertTrue(ok)
        with srv._state_lock:
            self.assertIsNone(srv._transcripts.get((id(b), "rid-x")),
                              "另一连接同 id 不得读到 A 的 transcript")
            self.assertEqual(srv._transcripts.get((id(a), "rid-x")),
                             "连接A的文本")
        srv.shutdown(timeout=1.0)

    # ---- 2. 上限与断连清理 ----
    def test_transcript_limits_and_disconnect_cleanup(self):
        srv = worker_main.WorkerServer(CONFIG, port=0)
        a = worker_main.ClientConn(None)
        big = "字" * (srv.MAX_TRANSCRIPT_ENTRY_BYTES + 10)
        self.assertFalse(srv._set_transcript(a, "r1", big), "单条超限被拒")
        # 总字节上限
        srv._transcripts.clear()
        chunk = "字" * 1024                     # 1KB 一条
        n = srv.MAX_TRANSCRIPT_TOTAL_BYTES // (1024 * 3) + 5
        accepted = 0
        for i in range(min(n, srv.MAX_TRANSCRIPT_ENTRIES)):
            if srv._set_transcript(a, f"r-{i}", chunk):
                accepted += 1
        self.assertLess(accepted, n, "总字节上限生效 (部分写入被拒)")
        # 断连清理
        srv._register_turn(a, "rt")
        srv._cancel_connection_turns(a)
        with srv._state_lock:
            self.assertIsNone(srv._transcripts.get((id(a), "rt")),
                              "断连清理该连接 transcript")
        srv.shutdown(timeout=1.0)

    # ---- 3. 阻塞 Provider 下 shutdown 兑现 deadline ----
    def test_blocking_provider_shutdown_deadline(self):
        class BlockingRunner:
            _audio_stop = None

            def __init__(self, config):
                pass

            def set_transcript(self, rid, text):
                pass

            def set_ai_mode(self, mode):
                pass                     # R8_R4_R3_R4: TurnRunner 新接口

            def run(self, rid, cancel_event, send):
                time.sleep(30)          # 同步阻塞, 不响应 cancel (模拟无超时 HTTP)

        srv = worker_main.WorkerServer(CONFIG, port=0)
        srv._runner_factory = BlockingRunner   # 测试注入点
        t0 = time.monotonic()
        # 直接提交一个阻塞 turn
        a = worker_main.ClientConn(None)
        srv._register_turn(a, "block")
        srv._turn_worker.submit(srv._run_turn_job, a, "block", "你好")
        time.sleep(0.2)
        result = srv.shutdown(timeout=0.5)   # deadline 必须生效
        elapsed = time.monotonic() - t0
        self.assertLess(elapsed, 3.0, "shutdown 兑现 deadline (不无限等待)")
        self.assertFalse(result["graceful"], "阻塞 turn 下不得伪称 graceful")
        self.assertFalse(result["turn_finished"])

    # ---- 4. 空闲客户端被 shutdown 关闭 ----
    def test_idle_client_closed_on_shutdown(self):
        port = _free_port()
        srv = worker_main.WorkerServer(CONFIG, port=port)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        time.sleep(0.3)
        c = socket.create_connection(("127.0.0.1", port), timeout=5)
        c.recv(4096)                       # hello
        result = srv.shutdown(timeout=2.0)
        c.settimeout(2.0)
        eof = False
        try:
            while True:
                d = c.recv(4096)
                if d == b"":
                    eof = True
                    break
        except (ConnectionResetError, socket.timeout, OSError):
            eof = True
        c.close()
        self.assertTrue(eof, "shutdown 主动关闭已接受的空闲客户端")
        self.assertTrue(result["graceful"])

    # ---- 5. main finally 调 shutdown ----
    def test_main_finally_calls_shutdown(self):
        calls = {"n": 0}
        orig = worker_main.WorkerServer.shutdown

        def fake_shutdown(self, timeout=5.0):
            calls["n"] += 1
            return {"graceful": True, "pending_readers": 0,
                    "turn_finished": True, "timed_out": False}

        class _Boom(Exception):
            pass

        worker_main.WorkerServer.shutdown = fake_shutdown
        try:
            def boom_serve(self):
                raise _Boom()
            orig_serve = worker_main.WorkerServer.serve_forever
            worker_main.WorkerServer.serve_forever = boom_serve
            try:
                try:
                    worker_main.main(["--fake", "--port", str(_free_port())])
                except _Boom:
                    pass
            finally:
                worker_main.WorkerServer.serve_forever = orig_serve
        finally:
            worker_main.WorkerServer.shutdown = orig
        self.assertGreaterEqual(calls["n"], 1, "异常路径 finally 调 shutdown")

    # ---- 6. 本地 HTTP mock: 请求格式 / 大小上限 / 超时 ----
    def test_local_http_provider_timeout_cancel_size_limit(self):
        srv = _QuietHTTPServer(("127.0.0.1", 0), _MockHttp)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        port = srv.server_address[1]
        os.environ["HOLOPET_API_KEY"] = "test-key-local"
        try:
            # 请求格式
            _MockHttp.payload_size = 10
            p = OpenAICompatibleProvider({
                "provider": "openai_compatible",
                "base_url": f"http://127.0.0.1:{port}/v1",
                "model": "m1", "request_timeout_ms": 2000})
            r = p.chat([{"role": "user", "content": "你好"}])
            self.assertEqual(r["content"], "x" * 10)
            self.assertTrue(_MockHttp.captured["path"].endswith(
                "/chat/completions"))
            body = json.loads(_MockHttp.captured["body"])
            self.assertEqual(body["model"], "m1")
            self.assertIn("messages", body)
            self.assertIn("Bearer test-key-local", _MockHttp.captured["auth"])

            # 大小上限
            _MockHttp.payload_size = 1024
            p2 = OpenAICompatibleProvider({
                "provider": "openai_compatible",
                "base_url": f"http://127.0.0.1:{port}/v1",
                "max_response_bytes": 128, "request_timeout_ms": 2000})
            with self.assertRaises(RuntimeError):
                p2.chat([{"role": "user", "content": "hi"}])

            # 超时
            _MockHttp.delay_s = 2.0
            p3 = OpenAICompatibleProvider({
                "provider": "openai_compatible",
                "base_url": f"http://127.0.0.1:{port}/v1",
                "request_timeout_ms": 300})
            with self.assertRaises(RuntimeError):
                p3.chat([{"role": "user", "content": "hi"}])
        finally:
            _MockHttp.delay_s = 0.0
            srv.shutdown()
            srv.server_close()
            os.environ.pop("HOLOPET_API_KEY", None)


if __name__ == "__main__":
    unittest.main()
