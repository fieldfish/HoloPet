"""test_r83_r3_r2_volc.py — R8_R3_R3_R2 必修 A §3.3 测试 (本地替身, 无公网)。

覆盖:
  1. websockets 14.1 正式路径只收到 additional_headers (旧参数名绝不进入);
  2. 旧版签名适配器仍可用 extra_headers (仅 <14 分支);
  3. 客户端签名不兼容/TypeError 归类 volc_client_incompatible, 绝不记网络故障;
  4. HTTP 403/401/400 握手拒绝 → volc_capability_not_enabled/volc_auth_rejected/
     volc_handshake_rejected, 保留脱敏状态码;
  5. 火山成功 → 备用 0 次; 可降级失败 (细分类) → 备用恰好 1 次; transcript 恰好 1;
  6. 预检日志包含非秘密字段且不含任何凭据值。
"""
import asyncio
import io
import json
import logging
import os
import struct
import sys
import threading
import unittest
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd.providers.stt import (OpenAiCompatSttProvider,  # noqa: E402
                                          SttError, TranscriptResult)
from holopet_agentd.providers.tts import OpenAiCompatTtsProvider  # noqa: E402
from holopet_agentd.providers.volcengine_streaming import (  # noqa: E402
    VolcengineStreamingSttProvider,
    ERR_VOLC_CLIENT_INCOMPAT, ERR_VOLC_CAPABILITY, ERR_VOLC_AUTH_REJECTED,
    ERR_VOLC_HANDSHAKE_REJECTED, ERR_VOLC_NETWORK, ERR_VOLC_CONFIG_MISSING)

from holopet_agentd.tests.test_r83_r3_contracts import (Resp, VolcFake,  # noqa: E402
                                                         FakeCapture, FakePlayer,
                                                         voice_config, make_wav,
                                                         DUMMY, ASR_ENV, TTS_ENV)

APP_ENV = "HOLOPET_R83_R3_R2_VOLC_APP"
ACC_ENV = "HOLOPET_R83_R3_R2_VOLC_ACC"
RES_ENV = "HOLOPET_R83_R3_R2_VOLC_RES"
SECRET_APP = "secret-app-value-0123456789abcdef"
SECRET_ACC = "secret-acc-value-0123456789abcdef"
SECRET_RES = "secret-res-value-0123456789abcdef"


class _FakeWs:
    def __init__(self, owner):
        self._owner = owner
        self.closed = 0

    async def send(self, data):
        self._owner.sent.append(data)

    async def recv(self):
        raise asyncio.TimeoutError()

    async def close(self):
        self.closed += 1


class _FakeInvalidStatus(Exception):
    def __init__(self, response):
        self.response = response


class _FakeResponse:
    def __init__(self, status_code):
        self.status_code = status_code


class _FakeWsModule14:
    """签名与 websockets 14.1 一致: connect(uri, *, additional_headers, ...)"""
    __version__ = "14.1"
    exceptions = type("exceptions", (), {"InvalidStatus": _FakeInvalidStatus})

    def __init__(self, raise_on_connect=None):
        self.calls = []
        self.raise_on_connect = raise_on_connect
        self.sent = []
        self.ws = None

    async def connect(self, uri, additional_headers=None, open_timeout=10,
                      close_timeout=10, **kwargs):
        self.calls.append({"uri": uri, "kwargs": dict(kwargs),
                           "additional_headers": additional_headers})
        if self.raise_on_connect is not None:
            raise self.raise_on_connect
        self.ws = _FakeWs(self)
        return self.ws


class _FakeWsModuleLegacy:
    """websockets<14 签名: connect(uri, *, extra_headers, ...)"""
    __version__ = "13.0"
    exceptions = type("exceptions", (), {"InvalidStatus": _FakeInvalidStatus})

    def __init__(self):
        self.calls = []
        self.sent = []
        self.ws = None

    async def connect(self, uri, extra_headers=None, **kwargs):
        self.calls.append({"extra_headers": extra_headers})
        self.ws = _FakeWs(self)
        return self.ws


class _FakeWsModuleUnknown:
    """两代参数名都不存在 → 客户端不兼容"""
    __version__ = "99.0"

    @staticmethod
    async def connect(uri, other=None, **kwargs):
        return _FakeWs(None)


def _provider_with(module, cfg=None):
    cfg = cfg or {"endpoint": "wss://fake.invalid/api/v3/sauc/bigmodel",
                  "app_key_env_name": APP_ENV,
                  "access_key_env_name": ACC_ENV,
                  "resource_id_env_name": RES_ENV}
    os.environ[APP_ENV] = SECRET_APP
    os.environ[ACC_ENV] = SECRET_ACC
    os.environ[RES_ENV] = SECRET_RES
    real = sys.modules.get("websockets")
    sys.modules["websockets"] = module
    p = VolcengineStreamingSttProvider(cfg)
    try:
        yield p
    finally:
        if real is not None:
            sys.modules["websockets"] = real
        else:
            sys.modules.pop("websockets", None)
        for k in (APP_ENV, ACC_ENV, RES_ENV):
            os.environ.pop(k, None)
        p.close()


class Volc14Compat(unittest.TestCase):
    def _start(self, module):
        gen = _provider_with(module)
        p = next(gen)
        st = 0
        try:
            err = p.start()
            st = p.stats().http_status
            return p, err, module, st
        finally:
            try:
                next(gen)
            except StopIteration:
                pass

    def test_14x_uses_additional_headers_only(self):
        mod = _FakeWsModule14()
        p, err, _, _ = self._start(mod)
        self.assertIsNone(err)
        self.assertEqual(1, len(mod.calls))
        self.assertIn("additional_headers", mod.calls[0])
        self.assertNotIn("extra_headers", mod.calls[0]["kwargs"])

    def test_legacy_signature_uses_extra_headers(self):
        mod = _FakeWsModuleLegacy()
        p, err, _, _ = self._start(mod)
        self.assertIsNone(err)
        self.assertEqual(1, len(mod.calls))
        self.assertIn("extra_headers", mod.calls[0])

    def test_unknown_signature_incompatible(self):
        p, err, _, _ = self._start(_FakeWsModuleUnknown())
        self.assertEqual(ERR_VOLC_CLIENT_INCOMPAT, err)

    def test_client_typeerror_not_network(self):
        p, err, _, _ = self._start(_FakeWsModule14(raise_on_connect=TypeError("x")))
        self.assertEqual(ERR_VOLC_CLIENT_INCOMPAT, err)
        self.assertNotEqual(ERR_VOLC_NETWORK, err)

    def test_http403_capability_not_enabled(self):
        p, err, _, st = self._start(
            _FakeWsModule14(raise_on_connect=_FakeInvalidStatus(
                _FakeResponse(403))))
        self.assertEqual(ERR_VOLC_CAPABILITY, err)
        self.assertEqual(403, st)

    def test_http401_auth_rejected(self):
        p, err, _, st = self._start(
            _FakeWsModule14(raise_on_connect=_FakeInvalidStatus(
                _FakeResponse(401))))
        self.assertEqual(ERR_VOLC_AUTH_REJECTED, err)
        self.assertEqual(401, st)

    def test_http400_handshake_rejected(self):
        p, err, _, st = self._start(
            _FakeWsModule14(raise_on_connect=_FakeInvalidStatus(
                _FakeResponse(400))))
        self.assertEqual(ERR_VOLC_HANDSHAKE_REJECTED, err)
        self.assertEqual(400, st)

    def test_oserror_network_unreachable(self):
        p, err, _, _ = self._start(_FakeWsModule14(raise_on_connect=OSError("dns")))
        self.assertEqual(ERR_VOLC_NETWORK, err)

    def test_missing_credentials_config_missing(self):
        for k in (APP_ENV, ACC_ENV, RES_ENV):
            os.environ.pop(k, None)
        p = VolcengineStreamingSttProvider(
            {"app_key_env_name": APP_ENV, "access_key_env_name": ACC_ENV,
             "resource_id_env_name": RES_ENV})
        try:
            self.assertEqual(ERR_VOLC_CONFIG_MISSING, p.start())
        finally:
            p.close()

    def test_preflight_log_fields_and_no_credential_leak(self):
        mod = _FakeWsModule14()
        with self.assertLogs("holopet_agentd", level="INFO") as captured:
            gen = _provider_with(mod)
            p = next(gen)
            try:
                p.start()
            finally:
                try:
                    next(gen)
                except StopIteration:
                    pass
        text = "\n".join(captured.output)
        self.assertIn("websockets_version=", text)
        self.assertIn("header_parameter=additional_headers", text)
        self.assertIn("endpoint_host=", text)
        self.assertIn("resource_id_present=True", text)
        for secret in (SECRET_APP, SECRET_ACC, SECRET_RES):
            self.assertNotIn(secret, text)


class AudioCrashCleanup(unittest.TestCase):
    """R8_R3_R3_R2: run() 内异常必须兜底取消采集 (不留 /tmp 残留)。"""

    def test_crash_calls_capture_cancel(self):
        calls = {"cancel": 0, "err": 0}
        orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
                worker_main.make_stt_provider, worker_main.make_tts_provider)

        class ExplodingCapture(FakeCapture):
            def read_incremental(self):
                raise RuntimeError("boom")

            def cancel(self):
                calls["cancel"] += 1

        class FeedVolc(VolcFake):
            def feed(self, pcm):
                raise RuntimeError("boom-feed")

        worker_main.make_audio_capture = lambda c: ExplodingCapture()
        worker_main.make_audio_player = lambda c: FakePlayer()
        worker_main.make_stt_provider = lambda c: OpenAiCompatSttProvider(
            c, opener=lambda req, timeout=None: Resp(b"{}"))
        worker_main.make_tts_provider = lambda c: OpenAiCompatTtsProvider(
            c, opener=lambda req, timeout=None: Resp(make_wav(), "audio/wav"))
        try:
            os.environ[ASR_ENV] = DUMMY
            os.environ[TTS_ENV] = DUMMY
            runner = worker_main.TurnRunner(voice_config())
            runner._volc_asr = FeedVolc()
            runner._audio_stop = threading.Event()
            try:
                runner.run("t1", threading.Event(), lambda *a, **k: True)
            except Exception:
                pass
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player,                 worker_main.make_stt_provider, worker_main.make_tts_provider = orig
            os.environ.pop(ASR_ENV, None)
            os.environ.pop(TTS_ENV, None)
        runner.abort_audio()          # 崩溃兜底出口 (与 _run_turn_job 等价)
        self.assertGreaterEqual(calls["cancel"], 1,
                                "异常路径必须取消采集并清理临时文件")


class VolcResponseParsing(unittest.TestCase):
    """R8_R3_R3_R2: 服务端响应的文本在 result.text (嵌套) — 真机曾因
    只认顶层 text 而 0 partials / empty_transcript。"""

    def test_growing_partials_final_is_last(self):
        """R8_R4_R3_R4_R5_R3 (v8): VOLC partial 是全文覆盖式渐进细化 —
        final 必须取最新 partial, 不得拼接全部 (拼接会产生
        "启动启动三启动3分钟…" 乱码; Pi 19:02 实测转录)。"""
        import gzip as _gzip, struct as _struct
        sys.path.insert(0, "agent")
        from holopet_agentd.providers.volcengine_streaming import (
            VolcengineStreamingSttProvider)

        def srv_frame(doc, flags=0, seq=None):
            payload = _gzip.compress(json.dumps(doc).encode("utf-8"))
            mid = _struct.pack(">i", seq) if seq is not None else b""
            return (bytes([0x11, 0x90 | flags, 0x11, 0x00]) + mid
                    + _struct.pack(">I", len(payload)) + payload)

        class _FakeWsModule14:
            pass

        # 渐进细化帧序列 (同 Pi 实测形态)
        frames = ["启动", "启动三", "启动3分钟", "启动3分钟定时器",
                  "启动3分钟定时器。"]
        mod = _FakeWsModule14()
        p = None
        os.environ[APP_ENV] = "a"; os.environ[ACC_ENV] = "b"
        os.environ[RES_ENV] = "c"
        real = sys.modules.get("websockets")
        sys.modules["websockets"] = mod
        try:
            p = VolcengineStreamingSttProvider(
                {"app_key_env_name": APP_ENV, "access_key_env_name": ACC_ENV,
                 "resource_id_env_name": RES_ENV})
            sess = p._session()
            out = None
            for i, t in enumerate(frames):
                last = (i == len(frames) - 1)
                out = sess._handle(
                    srv_frame({"result": {"text": t}},
                              flags=3 if last else 1,
                              seq=-(i + 1) if last else i + 1))
            self.assertFalse(out, "负序号最终帧必须结束会话")
            self.assertTrue(sess._finalized)
            self.assertEqual("启动3分钟定时器。", sess._final_text)
        finally:
            if real is not None:
                sys.modules["websockets"] = real
            else:
                sys.modules.pop("websockets", None)
            for k in (APP_ENV, ACC_ENV, RES_ENV):
                os.environ.pop(k, None)
            if p is not None:
                p.close()

    def test_result_text_nested_is_extracted(self):
        import gzip as _gzip, struct as _struct
        sys.path.insert(0, "agent")
        from holopet_agentd.providers.volcengine_streaming import (
            VolcengineStreamingSttProvider)

        class Ws:
            def __init__(self, owner):
                self._owner = owner

            async def send(self, d):
                pass

            async def recv(self):
                raise __import__("asyncio").TimeoutError()

            async def close(self):
                pass

        def srv_frame(doc, flags=0, seq=None):
            payload = _gzip.compress(json.dumps(doc).encode("utf-8"))
            mid = _struct.pack(">i", seq) if seq is not None else b""
            return (bytes([0x11, 0x90 | flags, 0x11, 0x00]) + mid
                    + _struct.pack(">I", len(payload)) + payload)

        mod = _FakeWsModule14()
        p = None
        os.environ[APP_ENV] = "a"; os.environ[ACC_ENV] = "b"
        os.environ[RES_ENV] = "c"
        real = sys.modules.get("websockets")
        sys.modules["websockets"] = mod
        try:
            p = VolcengineStreamingSttProvider(
                {"app_key_env_name": APP_ENV, "access_key_env_name": ACC_ENV,
                 "resource_id_env_name": RES_ENV})
            # 部分结果: result.text 嵌套
            out1 = p._session()._handle(
                srv_frame({"result": {"text": "你好"}}, flags=1, seq=3))
            self.assertTrue(out1)
            # 最终结果: 负序号 + result.text
            out2 = p._session()._handle(
                srv_frame({"result": {"text": "你好世界"}}, flags=3, seq=-4))
            self.assertFalse(out2, "负序号最终帧必须结束会话")
            st = p.stats()
            self.assertEqual(2, st.partials)
            # final_ms 依赖真实 _connect 的 _t0; 本测试只验解析与终态
        finally:
            if real is not None:
                sys.modules["websockets"] = real
            else:
                sys.modules.pop("websockets", None)
            for k in (APP_ENV, ACC_ENV, RES_ENV):
                os.environ.pop(k, None)
            if p is not None:
                p.close()


class VolcFeedProtocol(unittest.TestCase):
    """R8_R3_R3_R2: feed 内的 _drain 必须是 await 的普通协程 —
    真机曾因 async for 误用导致首包后 volc_protocol_error。"""

    def test_feed_sends_and_drains_without_protocol_error(self):
        import sys as _sys
        sys.path.insert(0, "agent")
        from holopet_agentd.providers.volcengine_streaming import (
            VolcengineStreamingSttProvider)

        class Ws:
            def __init__(self):
                self.sent = []

            async def send(self, d):
                self.sent.append(d)

            async def recv(self):
                raise __import__("asyncio").TimeoutError()

            async def close(self):
                pass

        mod = _FakeWsModule14()
        p = None
        os.environ[APP_ENV] = "a"; os.environ[ACC_ENV] = "b"
        os.environ[RES_ENV] = "c"
        real = _sys.modules.get("websockets")
        _sys.modules["websockets"] = mod
        try:
            p = VolcengineStreamingSttProvider(
                {"app_key_env_name": APP_ENV, "access_key_env_name": ACC_ENV,
                 "resource_id_env_name": RES_ENV})
            self.assertIsNone(p.start())
            p.feed(bytes([0, 1]) * 1600)
            st = p.stats()
            self.assertEqual(1, st.chunks_sent)
            self.assertEqual(3200, st.bytes_sent)
            self.assertFalse(p.last_error(),
                             "feed 后不得出现 volc_protocol_error")
        finally:
            if real is not None:
                _sys.modules["websockets"] = real
            else:
                _sys.modules.pop("websockets", None)
            for k in (APP_ENV, ACC_ENV, RES_ENV):
                os.environ.pop(k, None)
            if p is not None:
                p.close()


class VolcRealFeedTypes(unittest.TestCase):
    """R8_R3_R3_R2: 真机回归 — capture.read_incremental() 的 bytes 必须
    原样进入 volc.feed (不得对 bytes 迭代产生 int)。"""

    def test_feed_receives_bytes_not_int(self):
        fed = []
        events = []
        orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
                worker_main.make_stt_provider, worker_main.make_tts_provider)

        class ChunkedCapture(FakeCapture):
            def read_incremental(self):
                return b""

        class FeedSpy(VolcFake):
            def feed(self, pcm):
                fed.append(pcm)

        worker_main.make_audio_capture = lambda c: ChunkedCapture()
        worker_main.make_audio_player = lambda c: FakePlayer()
        worker_main.make_stt_provider = lambda c: OpenAiCompatSttProvider(
            c, opener=lambda req, timeout=None: Resp(
                json.dumps({"text": "备用"}).encode()))
        worker_main.make_tts_provider = lambda c: OpenAiCompatTtsProvider(
            c, opener=lambda req, timeout=None: Resp(make_wav(), "audio/wav"))
        try:
            os.environ[ASR_ENV] = DUMMY
            os.environ[TTS_ENV] = DUMMY
            runner = worker_main.TurnRunner(voice_config())
            runner._volc_asr = FeedSpy()
            runner._audio_stop = threading.Event()
            runner._audio_stop.set()
            runner.run("t1", threading.Event(),
                       lambda r, t, **f: (events.append((t, f)), True)[1])
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player,                 worker_main.make_stt_provider, worker_main.make_tts_provider = orig
            os.environ.pop(ASR_ENV, None)
            os.environ.pop(TTS_ENV, None)
        self.assertTrue(fed, "火山会话必须收到增量 PCM")
        for chunk in fed:
            self.assertIsInstance(chunk, (bytes, bytearray),
                                  "feed 只能收到 bytes (真机 int 回归)")
        self.assertEqual(b"", fed[0])


class VolcFineCodeControlFlow(unittest.TestCase):
    """细分类错误码在真实 TurnRunner 控制流中的降级记账。"""

    def _run(self, fail_code):
        calls = {"stt": 0}
        events = []
        orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
                worker_main.make_stt_provider, worker_main.make_tts_provider)
        worker_main.make_audio_capture = lambda c: FakeCapture()
        worker_main.make_audio_player = lambda c: FakePlayer()

        class SpyStt(OpenAiCompatSttProvider):
            def transcribe(self, rid, wav, fmt):
                calls["stt"] += 1
                return TranscriptResult(text="备用识别结果", confidence=1.0)

        worker_main.make_stt_provider = lambda c: SpyStt(c)
        worker_main.make_tts_provider = lambda c: OpenAiCompatTtsProvider(
            c, opener=lambda req, timeout=None: Resp(make_wav(), "audio/wav"))
        try:
            os.environ[ASR_ENV] = DUMMY
            os.environ[TTS_ENV] = DUMMY
            runner = worker_main.TurnRunner(voice_config())
            runner._volc_asr = VolcFake(fail_code=fail_code, fail_at="finish")
            runner._audio_stop = threading.Event()
            runner._audio_stop.set()
            runner.run("t1", threading.Event(),
                       lambda r, t, **f: (events.append((t, f)), True)[1])
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player, \
                worker_main.make_stt_provider, worker_main.make_tts_provider = orig
            os.environ.pop(ASR_ENV, None)
            os.environ.pop(TTS_ENV, None)
        return calls, events, runner

    def test_fine_error_fallback_once_with_reason(self):
        calls, events, runner = self._run("volc_capability_not_enabled")
        self.assertEqual(1, calls["stt"])
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))
        m = runner._turn_metrics
        self.assertEqual("yes", m.get("ASR_FALLBACK_USED"))
        self.assertEqual("volc_capability_not_enabled", m.get("ASR_FALLBACK_REASON"))
        self.assertEqual("volcengine_streaming", m.get("ASR_PRIMARY"))

    def test_volc_success_zero_fallback_fine_path(self):
        calls, events, _ = self._run(None)
        self.assertEqual(0, calls["stt"])
        self.assertEqual(1, len([t for t, _ in events if t == "transcript"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
