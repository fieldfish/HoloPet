"""test_provider_errors.py — R8_R2 工作包 B: Provider 稳定错误类别。

全部使用本机假 HTTP server (不访问公网), 覆盖:
  正常文本回答 / 工具调用结构 / 401 / 403 / 429 / 5xx / 断连 /
  超时 / 超大响应 / 非 JSON / 缺 choices / 空 choices / 错误 content 类型 /
  缺 Key (不静默回退) / 日志与异常消息不含测试 Key。
"""

import http.server
import json
import logging
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.providers import make_provider  # noqa: E402
from holopet_agentd.providers.errors import (  # noqa: E402
    AuthError, MalformedResponseError, MissingApiKeyError, ModelBlockedError,
    NetworkError, ProviderConfigError, RateLimitedError,
    ResponseTooLargeError, TimeoutError_, event_error_code,
)

TEST_KEY = "unit-test-placeholder"


class _Handler(http.server.BaseHTTPRequestHandler):
    mode = "ok"
    body = None

    def log_message(self, *a):   # 静默 (测试输出干净)
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        m = type(self).mode
        if m == "ok":
            payload = {"choices": [{"message": {"content": "hello 你好"},
                                    "finish_reason": "stop"}]}
            self._json(200, payload)
        elif m == "tool":
            payload = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{"id": "c1", "function":
                                {"name": "get_time", "arguments": "{}"}}]},
                "finish_reason": "tool_calls"}]}
            self._json(200, payload)
        elif m == "401":
            self._json(401, {"error": "denied"})
        elif m == "403":
            self._json(403, {"error": "denied"})
        elif m == "429":
            self._json(429, {"error": "slow down"})
        elif m == "400model":
            # R8_R4_R3_R4_R5_R3: 服务端点名拒绝模型
            self._json(400, {"error": {"message": "Model Not Exist",
                                       "type": "invalid_request_error"}})
        elif m == "400other":
            self._json(400, {"error": "bad request payload"})
        elif m == "500":
            self._json(500, {"error": "boom"})
        elif m == "notjson":
            self._raw(200, b"<html>not json</html>")
        elif m == "nochoices":
            self._json(200, {"id": "x"})
        elif m == "emptychoices":
            self._json(200, {"choices": []})
        elif m == "badcontent":
            self._json(200, {"choices": [{"message": {"content": 42}}]})
        elif m == "huge":
            big = "x" * (300 * 1024)
            self._json(200, {"choices": [{"message": {"content": big}}]})
        elif m == "hang":
            time.sleep(3)
            self._json(200, {"choices": [{"message": {"content": "late"}}]})
        elif m == "close":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n")
            self.wfile.flush()
            self.connection.close()
        else:
            self._json(500, {"error": "unknown mode"})

    def _json(self, code, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _raw(self, code, data):
        self.send_response(code)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class PureLocalErrorMapping(unittest.TestCase):
    """无需 server 的纯映射检查。"""

    def test_event_code_map(self):
        self.assertEqual(event_error_code(MissingApiKeyError("x")),
                         "llm_missing_key")
        self.assertEqual(event_error_code(AuthError("x")), "llm_auth_error")
        self.assertEqual(event_error_code(RateLimitedError("x")),
                         "llm_rate_limited")
        self.assertEqual(event_error_code(TimeoutError_("x")), "llm_timeout")
        self.assertEqual(event_error_code(NetworkError("x")),
                         "llm_network_error")
        self.assertEqual(event_error_code(ResponseTooLargeError("x")),
                         "llm_response_too_large")
        self.assertEqual(event_error_code(MalformedResponseError("x")),
                         "llm_malformed_response")
        self.assertEqual(event_error_code(ProviderConfigError("x")),
                         "llm_config_error")
        self.assertEqual(event_error_code(ModelBlockedError("x")),
                         "PROVIDER_MODEL_BLOCKED")
        self.assertEqual(event_error_code(ValueError("x")), "llm_failed")


class _QuietThreadingServer(http.server.ThreadingHTTPServer):
    """R8_R2: 超时用例中客户端先断开属预期 — 仅静默三类断连异常,
    其余异常仍走默认处理 (与 R7_R1 _QuietHTTPServer 同策略);
    避免 socketserver 把含解释器绝对路径的 traceback 打进正式日志。"""

    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionAbortedError, ConnectionResetError,
                            BrokenPipeError)):
            return
        super().handle_error(request, client_address)


class TestProviderAgainstFakeServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = _QuietThreadingServer(
            ("127.0.0.1", 0), _Handler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        os.environ["HOLOPET_R8R2_TEST_KEY"] = TEST_KEY
        # 捕获 agentd 全部日志供"日志不含 Key"断言
        self.log_records = []
        handler = logging.Handler()
        handler.emit = lambda r: self.log_records.append(r.getMessage())
        root = logging.getLogger("holopet_agentd")
        old_level = root.level
        root.setLevel(logging.INFO)          # info 级日志 (api_key_present) 可见
        root.addHandler(handler)
        def _restore():
            root.removeHandler(handler)
            root.setLevel(old_level)
        self.addCleanup(_restore)

    def tearDown(self):
        os.environ.pop("HOLOPET_R8R2_TEST_KEY", None)

    def _provider(self, mode, **over):
        _Handler.mode = mode
        cfg = {
            "provider": "openai_compatible",
            "base_url": f"http://127.0.0.1:{self.port}/v1",
            "model": "test-model",
            "api_key_env_name": "HOLOPET_R8R2_TEST_KEY",
            "request_timeout_ms": 800,
            "max_response_bytes": 200 * 1024,
        }
        cfg.update(over)
        return make_provider(cfg)

    def test_ok_text(self):
        r = self._provider("ok").chat([{"role": "user", "content": "hi"}])
        self.assertEqual(r["content"], "hello 你好")
        self.assertEqual(r["tool_calls"], [])

    def test_tool_call_structure(self):
        r = self._provider("tool").chat([{"role": "user", "content": "t"}])
        self.assertEqual(r["tool_calls"][0]["function"]["name"], "get_time")

    def test_401_auth_error(self):
        with self.assertRaises(AuthError):
            self._provider("401").chat([])

    def test_403_auth_error(self):
        with self.assertRaises(AuthError):
            self._provider("403").chat([])

    def test_429_rate_limited(self):
        with self.assertRaises(RateLimitedError):
            self._provider("429").chat([])

    def test_500_network_error(self):
        with self.assertRaises(NetworkError):
            self._provider("500").chat([])

    def test_not_json_malformed(self):
        with self.assertRaises(MalformedResponseError):
            self._provider("notjson").chat([])

    def test_missing_choices(self):
        with self.assertRaises(MalformedResponseError):
            self._provider("nochoices").chat([])

    def test_empty_choices(self):
        with self.assertRaises(MalformedResponseError):
            self._provider("emptychoices").chat([])

    def test_bad_content_type(self):
        with self.assertRaises(MalformedResponseError):
            self._provider("badcontent").chat([])

    def test_huge_response_too_large(self):
        with self.assertRaises(ResponseTooLargeError):
            self._provider("huge").chat([])

    def test_timeout(self):
        with self.assertRaises(TimeoutError_):
            self._provider("hang", request_timeout_ms=300).chat([])

    def test_connection_closed_network_error(self):
        with self.assertRaises((NetworkError, MalformedResponseError)):
            self._provider("close").chat([])

    def test_missing_key_no_silent_fallback(self):
        os.environ.pop("HOLOPET_R8R2_TEST_KEY", None)
        with self.assertRaises(MissingApiKeyError):
            self._provider("ok").chat([])

    def test_config_error_missing_base_url(self):
        _Handler.mode = "ok"
        p = make_provider({"provider": "openai_compatible", "base_url": "",
                           "api_key_env_name": "HOLOPET_R8R2_TEST_KEY"})
        with self.assertRaises(ProviderConfigError):
            p.chat([])

    def test_logs_never_contain_key(self):
        # 跑几类失败 + 成功, 扫描全部日志消息
        for mode in ("ok", "401", "429", "notjson"):
            try:
                self._provider(mode).chat([])
            except Exception:      # noqa: BLE001 — 断言只看日志
                pass
        joined = "\n".join(self.log_records)
        self.assertNotIn(TEST_KEY, joined)
        self.assertNotIn(TEST_KEY[:12], joined)
        self.assertIn("api_key_present=yes", joined)

    def test_400_model_rejection_is_model_blocked(self):
        with self.assertRaises(ModelBlockedError):
            self._provider("400model").chat([])

    def test_400_other_is_config_error(self):
        with self.assertRaises(ProviderConfigError):
            self._provider("400other").chat([])

    def test_model_blocked_message_no_body(self):
        try:
            self._provider("400model").chat([])
            self.fail("应抛 ModelBlockedError")
        except ModelBlockedError as e:
            s = str(e)
            self.assertNotIn("Model Not Exist", s,
                             "错误消息不得包含远端正文")
            self.assertNotIn(TEST_KEY, s)

    def test_error_message_no_body_or_key(self):
        try:
            self._provider("401").chat([])
            self.fail("应抛 AuthError")
        except AuthError as e:
            s = str(e)
            self.assertNotIn("denied", s, "错误消息不得包含远端正文")
            self.assertNotIn(TEST_KEY, s)


if __name__ == "__main__":
    unittest.main()
