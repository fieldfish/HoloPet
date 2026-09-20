"""test_r83_stt_tts.py — R8_R3 真实 ASR/TTS 适配合同测试 (§7.3/§7.4)。

全部使用注入式本地 HTTP 替身: 不联网、不需要真实 Key (占位 Key 仅存在于
测试进程环境, 不得进入任何事件/日志/交付物)。
覆盖: 成功、空 transcript、401/403、429、5xx、超时、畸形响应、超大响应;
      TTS 成功/空音频/错误 MIME/超时/超大; 真实 RMS 包络来自实际 PCM。
"""

import io
import json
import os
import socket
import struct
import sys
import unittest
import urllib.error
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.providers.stt import (  # noqa: E402
    AudioFormat, OpenAiCompatSttProvider,
)
from holopet_agentd.providers.tts import (  # noqa: E402
    OpenAiCompatTtsProvider, rms_envelope, wav_pcm_payload,
)

DUMMY_KEY = "placeholder-key-for-unit-test-only"
ASR_ENV = "HOLOPET_R83_TEST_ASR_KEY"
TTS_ENV = "HOLOPET_R83_TEST_TTS_KEY"


def make_wav(seconds=0.5, rate=16000, amp=0):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        n = int(seconds * rate)
        w.writeframes(struct.pack("<%dh" % n, *([amp] * n)))
    return buf.getvalue()


class FakeResp:
    def __init__(self, body=b"", content_type="application/json"):
        self._body = body
        self.headers = {"Content-Type": content_type}

    def read(self, n=None):
        return self._body if n is None else self._body[:n]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def opener_ok(body, content_type="application/json"):
    seen = {}

    def _open(req, timeout=None):
        seen["url"] = getattr(req, "full_url", "")
        seen["body"] = getattr(req, "data", b"")
        seen["headers"] = dict(getattr(req, "headers", {}) or {})
        seen["timeout"] = timeout
        return FakeResp(body, content_type)

    _open.seen = seen
    return _open


def opener_http(code):
    def _open(req, timeout=None):
        raise urllib.error.HTTPError(getattr(req, "full_url", "u"), code,
                                     "err", {}, None)
    return _open


def opener_timeout():
    def _open(req, timeout=None):
        raise socket.timeout("timed out")
    return _open


class AsrTests(unittest.TestCase):
    def setUp(self):
        os.environ[ASR_ENV] = DUMMY_KEY
        self.addCleanup(lambda: os.environ.pop(ASR_ENV, None))
        self.fmt = AudioFormat(max_len_bytes=1024 * 1024)
        self.cfg = {"provider": "openai_compatible",
                    "base_url": "https://asr.invalid/v1", "model": "asr-test",
                    "api_key_env_name": ASR_ENV, "request_timeout_ms": 1234}

    def _p(self, opener):
        return OpenAiCompatSttProvider(self.cfg, opener=opener)

    def test_success_multipart_and_text(self):
        op = opener_ok(json.dumps({"text": "你好，我在"}).encode())
        p = self._p(op)
        res = p.transcribe("rid", make_wav(0.2, amp=100), self.fmt)
        self.assertIsNone(res.error, res)
        self.assertEqual("你好，我在", res.text)
        self.assertIn("/audio/transcriptions", op.seen["url"])
        self.assertIn(b"asr-test", op.seen["body"])
        self.assertIn(b"utterance.wav", op.seen["body"])
        self.assertIn(b"RIFF", op.seen["body"])
        self.assertEqual(1.234, op.seen["timeout"])

    def test_empty_transcript_is_stable_error(self):
        p = self._p(opener_ok(json.dumps({"text": "   "}).encode()))
        res = p.transcribe("rid", make_wav(0.1), self.fmt)
        self.assertEqual("no_transcript", res.error.code)

    def test_missing_key_config_and_auth_errors(self):
        os.environ.pop(ASR_ENV, None)
        res = self._p(opener_ok(b"{}")).transcribe("rid", make_wav(0.1), self.fmt)
        self.assertEqual("missing_api_key", res.error.code)
        os.environ[ASR_ENV] = DUMMY_KEY
        for code, want in ((401, "auth_error"), (403, "auth_error"),
                           (429, "rate_limited"), (500, "network_error"),
                           (400, "malformed_response")):
            res = self._p(opener_http(code)).transcribe("rid", make_wav(0.1),
                                                        self.fmt)
            self.assertEqual(want, res.error.code, code)

    def test_timeout_and_malformed_and_oversize(self):
        res = self._p(opener_timeout()).transcribe("rid", make_wav(0.1), self.fmt)
        self.assertEqual("timeout", res.error.code)
        res = self._p(opener_ok(b"not-json")).transcribe("rid", make_wav(0.1),
                                                         self.fmt)
        self.assertEqual("malformed_response", res.error.code)
        res = self._p(opener_ok(b'{"text": 123}')).transcribe("rid", make_wav(0.1),
                                                              self.fmt)
        self.assertEqual("malformed_response", res.error.code)
        cfg = dict(self.cfg)
        cfg["max_response_bytes"] = 8
        p = OpenAiCompatSttProvider(cfg, opener=opener_ok(b'{"text":"long tail"}'))
        res = p.transcribe("rid", make_wav(0.1), self.fmt)
        self.assertEqual("response_too_large", res.error.code)

    def test_local_preflight_errors(self):
        p = self._p(opener_ok(b'{"text":"x"}'))
        self.assertEqual("no_audio", p.transcribe("rid", b"", self.fmt).error.code)
        small = AudioFormat(max_len_bytes=10)
        self.assertEqual("too_large",
                         p.transcribe("rid", make_wav(0.5), small).error.code)
        bad = AudioFormat(sample_rate=1)
        self.assertEqual("bad_format",
                         p.transcribe("rid", make_wav(0.1), bad).error.code)
        p2 = OpenAiCompatSttProvider({"provider": "openai_compatible"},
                                     opener=opener_ok(b"{}"))
        self.assertEqual("config_error",
                         p2.transcribe("rid", make_wav(0.1), self.fmt).error.code)

    def test_no_key_leak_in_errors(self):
        for op in (opener_http(401), opener_timeout(), opener_ok(b"bad")):
            res = self._p(op).transcribe("rid", make_wav(0.1), self.fmt)
            self.assertNotIn(DUMMY_KEY, str(res.error))
            self.assertNotIn(DUMMY_KEY, res.error.message)


class TtsTests(unittest.TestCase):
    def setUp(self):
        os.environ[TTS_ENV] = DUMMY_KEY
        self.addCleanup(lambda: os.environ.pop(TTS_ENV, None))
        self.cfg = {"provider": "openai_compatible",
                    "base_url": "https://tts.invalid/v1", "model": "tts-test",
                    "voice": "voice-x", "api_key_env_name": TTS_ENV}

    def _p(self, opener, **kw):
        cfg = dict(self.cfg)
        cfg.update(kw)
        return OpenAiCompatTtsProvider(cfg, opener=opener)

    def test_success_real_audio_and_rms_from_pcm(self):
        wav = make_wav(0.6, amp=0)          # 静音 → 真实 RMS 必须为 0
        op = opener_ok(wav, "audio/wav")
        res = self._p(op).synthesize("rid", "你好", "voice-x")
        self.assertIsNone(res.error, res)
        self.assertEqual("audio/wav", res.mime)
        self.assertEqual(wav, res.audio)
        self.assertTrue(res.chunks)
        for c in res.chunks:
            self.assertEqual(0.0, c.rms, "静音音频的 RMS 必须来自真实 PCM=0")
        self.assertGreater(res.duration_ms, 400)
        self.assertIn("/audio/speech", op.seen["url"])
        body = json.loads(op.seen["body"].decode("utf-8"))
        self.assertEqual("你好", body["input"])
        self.assertEqual("voice-x", body["voice"])

    def test_loud_audio_has_positive_rms(self):
        wav = make_wav(0.3, amp=20000)
        res = self._p(opener_ok(wav, "audio/wav")).synthesize("r", "hi", "v")
        self.assertTrue(res.chunks)
        self.assertGreater(max(c.rms for c in res.chunks), 0.3)

    def test_errors_are_stable(self):
        self.assertEqual("empty_text",
                         self._p(opener_ok(b"x", "audio/wav")).synthesize(
                             "r", "", "v").error.code)
        self.assertEqual("text_too_long",
                         self._p(opener_ok(b"x", "audio/wav"),
                                 max_text_chars=2).synthesize(
                             "r", "toolong", "v").error.code)
        for code, want in ((401, "auth_error"), (429, "rate_limited"),
                           (503, "network_error")):
            self.assertEqual(want, self._p(opener_http(code)).synthesize(
                "r", "hi", "v").error.code, code)
        self.assertEqual("timeout",
                         self._p(opener_timeout()).synthesize("r", "hi", "v").error.code)
        self.assertEqual("malformed_response",
                         self._p(opener_ok(b'{"err":1}')).synthesize(
                             "r", "hi", "v").error.code)
        self.assertEqual("empty_audio",
                         self._p(opener_ok(b"", "audio/wav")).synthesize(
                             "r", "hi", "v").error.code)
        cfg = dict(self.cfg)
        cfg["max_response_bytes"] = 8
        self.assertEqual("response_too_large",
                         OpenAiCompatTtsProvider(
                             cfg, opener=opener_ok(b"0" * 64, "audio/wav")
                         ).synthesize("r", "hi", "v").error.code)
        os.environ.pop(TTS_ENV, None)
        self.assertEqual("missing_api_key",
                         self._p(opener_ok(b"x", "audio/wav")).synthesize(
                             "r", "hi", "v").error.code)

    def test_no_key_leak(self):
        res = self._p(opener_http(401)).synthesize("r", "hi", "v")
        self.assertNotIn(DUMMY_KEY, str(res.error))


class RmsEnvelopeTests(unittest.TestCase):
    def test_silence_and_loud_and_wrong_depth(self):
        self.assertEqual([], rms_envelope(b"", "audio/wav"))
        self.assertEqual(0.0, rms_envelope(make_wav(0.05, amp=0),
                                           "audio/wav")[0].rms)
        self.assertGreater(rms_envelope(make_wav(0.05, amp=30000),
                                        "audio/wav")[0].rms, 0.5)
        self.assertEqual([], rms_envelope(b"\x00" * 100, "audio/wav", bits=8))
        self.assertEqual(b"raw", wav_pcm_payload(b"raw"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
