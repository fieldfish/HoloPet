"""test_stt_tts.py — STT/TTS Provider 合同测试  V6-R2"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.providers.stt import (  # noqa: E402
    AudioFormat, FakeSttProvider, SttError, validate_audio_format)
from holopet_agentd.providers.tts import FakeTtsProvider  # noqa: E402


class TestStt(unittest.TestCase):
    def setUp(self):
        self.stt = FakeSttProvider({"t1": "现在几点", "t2": "你好"})

    def test_transcribe_by_request_id(self):
        r = self.stt.transcribe("t1", b"\x00\x01", AudioFormat())
        self.assertIsNone(r.error)
        self.assertEqual(r.text, "现在几点")
        r2 = self.stt.transcribe("t2", b"\x00\x01", AudioFormat())
        self.assertEqual(r2.text, "你好")

    def test_no_transcript_is_structured_error(self):
        r = self.stt.transcribe("unknown", b"\x00\x01", AudioFormat())
        self.assertIsNotNone(r.error)
        self.assertEqual(r.error.code, "no_transcript")

    def test_empty_audio_error(self):
        r = self.stt.transcribe("t1", b"", AudioFormat())
        self.assertEqual(r.error.code, "no_audio")

    def test_too_large_audio_error(self):
        fmt = AudioFormat(max_len_bytes=4)
        r = self.stt.transcribe("t1", b"\x00" * 8, fmt)
        self.assertEqual(r.error.code, "too_large")

    def test_bad_format_error(self):
        fmt = AudioFormat(sample_rate=1000)
        r = self.stt.transcribe("t1", b"\x00", fmt)
        self.assertEqual(r.error.code, "bad_format")
        self.assertIsNotNone(validate_audio_format(AudioFormat(sample_rate=1)))
        self.assertIsNone(validate_audio_format(AudioFormat()))


class TestTts(unittest.TestCase):
    def setUp(self):
        self.tts = FakeTtsProvider()

    def test_synthesize_has_verifiable_chunks(self):
        r = self.tts.synthesize("r1", "你好世界", "default")
        self.assertIsNone(r.error)
        self.assertGreater(len(r.chunks), 0)
        self.assertGreater(r.duration_ms, 0)
        # 电平确定性 (0..1)
        for c in r.chunks:
            self.assertGreaterEqual(c.rms, 0.0)
            self.assertLessEqual(c.rms, 1.0)

    def test_empty_text_error(self):
        r = self.tts.synthesize("r1", "", "default")
        self.assertEqual(r.error.code, "empty_text")

    def test_too_long_error(self):
        r = self.tts.synthesize("r1", "x" * 500, "default")
        self.assertEqual(r.error.code, "text_too_long")

    def test_deterministic(self):
        a = self.tts.synthesize("r1", "测试文本", "v")
        b = self.tts.synthesize("r1", "测试文本", "v")
        self.assertEqual([c.rms for c in a.chunks], [c.rms for c in b.chunks])


if __name__ == "__main__":
    unittest.main()
