"""test_r83_r3_contracts.py — R8_R3_R3 关键合同测试 (§8.1 关键项, 本地替身)。"""
import io
import json
import os
import struct
import sys
import threading
import unittest
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd.audio.capture import PcmResult  # noqa: E402
from holopet_agentd.audio.player import PlaybackResult  # noqa: E402
from holopet_agentd.providers.stt import (OpenAiCompatSttProvider,  # noqa: E402
                                          SttError, TranscriptResult)
from holopet_agentd.providers.tts import OpenAiCompatTtsProvider  # noqa: E402

ASR_ENV = "HOLOPET_R83_R3_ASR_KEY"
TTS_ENV = "HOLOPET_R83_R3_TTS_KEY"
DUMMY = "placeholder"


class Resp:
    def __init__(self, body, ctype="application/json"):
        self._b = body
        self.headers = {"Content-Type": ctype}

    def read(self, n=None):
        return self._b if n is None else self._b[:n]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def make_wav(seconds=0.3, amp=0):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        n = int(seconds * 16000)
        w.writeframes(struct.pack("<%dh" % n, *([amp] * n)))
    return buf.getvalue()


class FakeCapture:
    def __init__(self, wav=b""):
        self.wav = wav or make_wav()
        self.started = 0

    def config_reason(self):
        return None

    def start(self, rid):
        self.started += 1
        return True

    def read_incremental(self):
        return b""

    def stop(self):
        return PcmResult(wav_bytes=self.wav,
                         pcm_bytes=max(0, len(self.wav) - 44),
                         duration_ms=300, device_class="plughw",
                         exit_code=0, cleaned=True)

    def cancel(self):
        pass


class FakePlayer:
    def play(self, rid, audio, mime, cancel_event=None, estimator=None,
             on_started=None):
        if on_started:
            on_started()
        return PlaybackResult(played=True, duration_ms=300, exit_code=0,
                              cleaned=True)


class VolcFake:
    def __init__(self, fail_code=None, fail_at="start"):
        self._fail = fail_code
        self._fail_at = fail_at
        self.started = 0
        self.finished = 0
        self.cancelled = 0

    def start(self):
        self.started += 1
        if self._fail_at == "start" and self._fail:
            return self._fail
        return None

    def feed(self, pcm):
        pass

    def finish(self, timeout_ms=30000):
        self.finished += 1
        if self._fail_at == "finish" and self._fail:
            return TranscriptResult(error=SttError(self._fail, self._fail))
        return TranscriptResult(text="你好，这是火山识别", confidence=1.0)

    def stats(self):
        from holopet_agentd.providers.volcengine_streaming import StreamingStats
        return StreamingStats()

    def cancel(self):
        self.cancelled += 1

    def close(self):
        pass

    def last_error(self):
        return self._fail or ""


def voice_config(**over):
    cfg = {
        "provider": "fake", "text_only": False,
        "audio_capture": {"provider": "alsa", "device": "plughw:CARD=X,DEV=0",
                          "max_record_seconds": 3, "max_audio_bytes": 1048576},
        "audio_playback": {"provider": "alsa", "device": "plughw:CARD=X,DEV=0"},
        "asr": {"provider": "volcengine_streaming",
                "endpoint": "wss://fake.invalid/api/v3/sauc/bigmodel",
                "app_key_env_name": "VOLC_K", "access_key_env_name": "VOLC_A",
                "resource_id_env_name": "VOLC_R",
                "fallback_provider": "openai_compatible",
                "base_url": "https://asr.invalid/v1",
                "model": "FunAudioLLM/SenseVoiceSmall",
                "api_key_env_name": ASR_ENV,
                "max_fallbacks_per_turn": 1},
        "tts": {"provider": "openai_compatible",
                "base_url": "https://tts.invalid/v1", "model": "tts-x",
                "voice": "FunAudioLLM/CosyVoice2-0.5B:alex",
                "api_key_env_name": TTS_ENV},
        "response_policy": {"short_max_chars": 60},
    }
    cfg.update(over)
    return cfg


def run_turn(cfg, volc, fallback_text="备用识别结果", cancel=False):
    events = []
    orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
            worker_main.make_stt_provider, worker_main.make_tts_provider)
    worker_main.make_audio_capture = lambda c: FakeCapture()
    worker_main.make_audio_player = lambda c: FakePlayer()
    worker_main.make_stt_provider = lambda c: OpenAiCompatSttProvider(
        c, opener=lambda req, timeout=None: Resp(
            json.dumps({"text": fallback_text}).encode()))
    worker_main.make_tts_provider = lambda c: OpenAiCompatTtsProvider(
        c, opener=lambda req, timeout=None: Resp(make_wav(), "audio/wav"))
    try:
        os.environ[ASR_ENV] = DUMMY
        os.environ[TTS_ENV] = DUMMY
        runner = worker_main.TurnRunner(cfg)
        runner._volc_asr = volc
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        cancel_ev = threading.Event()
        if cancel:
            cancel_ev.set()
        runner.run("t1", cancel_ev,
                   lambda r, t, **f: (events.append((t, f)), True)[1])
    finally:
        worker_main.make_audio_capture, worker_main.make_audio_player, \
            worker_main.make_stt_provider, worker_main.make_tts_provider = orig
        os.environ.pop(ASR_ENV, None)
        os.environ.pop(TTS_ENV, None)
    return events


class R83R3Contracts(unittest.TestCase):
    def test_tts_request_uses_nested_voice(self):
        seen = {}

        def opener(req, timeout=None):
            body = json.loads(getattr(req, "data", b"{}").decode())
            seen["voice"] = body.get("voice")
            return Resp(make_wav(), "audio/wav")

        p = OpenAiCompatTtsProvider(voice_config()["tts"], opener=opener)
        os.environ[TTS_ENV] = DUMMY
        self.addCleanup(lambda: os.environ.pop(TTS_ENV, None))
        r = p.synthesize("r", "你好", "")   # TurnRunner 现在传嵌套 tts.voice
        self.assertIsNone(r.error)
        self.assertEqual("FunAudioLLM/CosyVoice2-0.5B:alex", seen["voice"])

    def test_volc_primary_no_fallback_on_success(self):
        volc = VolcFake()
        events = run_turn(voice_config(), volc)
        kinds = [t for t, _ in events]
        self.assertIn("transcript", kinds)
        self.assertIn("done", kinds)
        self.assertEqual(1, volc.finished)

    def test_volc_finish_error_falls_back_exactly_once(self):
        volc = VolcFake(fail_code="auth_error", fail_at="finish")
        events = run_turn(voice_config(), volc)
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))
        self.assertEqual("备用识别结果", tr[0]["text"])
        self.assertIn("done", [t for t, _ in events])

    def test_cancel_does_not_fallback(self):
        volc = VolcFake(fail_code="auth_error", fail_at="finish")
        events = run_turn(voice_config(), volc, cancel=True)
        self.assertEqual([], [t for t, _ in events if t == "transcript"])
        self.assertEqual([], [t for t, _ in events if t == "done"])

    def test_agentd_vlog_appends_to_audit_file(self):
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        self.addCleanup(lambda: os.path.exists(path) and os.unlink(path))
        cfg = voice_config()
        cfg["voice_audit"] = path
        runner = worker_main.TurnRunner(cfg)
        runner._vlog("rid9", "asr_provider_selected",
                     value="volcengine_streaming")
        doc = json.loads(open(path, encoding="utf-8").read().strip())
        self.assertEqual("obs", doc["event"])
        self.assertEqual("asr_provider_selected", doc["kind"])
        self.assertEqual("rid9", doc["request_id"])




class R83R3R1Contracts(unittest.TestCase):
    def test_missing_credentials_records_fallback_yes(self):
        volc = VolcFake(fail_code="missing_credentials", fail_at="start")
        events = run_turn(voice_config(), volc)
        metrics = [f for t, f in events if t == "obs" and f.get("kind") == "turn_metrics"]
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr), "备用识别必须恰好一次")
        self.assertEqual("备用识别结果", tr[0]["text"])
        self.assertIn("done", [t for t, _ in events])

    def test_volc_start_error_fallback_once(self):
        volc = VolcFake(fail_code="auth_error", fail_at="start")
        events = run_turn(voice_config(), volc)
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))

    def test_primary_success_no_fallback_count(self):
        volc = VolcFake()
        events = run_turn(voice_config(), volc)
        self.assertIn("done", [t for t, _ in events])
        self.assertEqual(1, volc.finished)




class R83R3R1CtrlFlow(unittest.TestCase):
    """补充授权单 §3.4: 真实 TurnRunner.run() 控制流 + 计数 spy。"""

    def _run(self, volc):
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
            runner._volc_asr = volc
            runner._audio_stop = threading.Event()
            runner._audio_stop.set()
            runner.run("t1", threading.Event(),
                       lambda r, t, **f: (events.append((t, f)), True)[1])
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player,                 worker_main.make_stt_provider, worker_main.make_tts_provider = orig
            os.environ.pop(ASR_ENV, None)
            os.environ.pop(TTS_ENV, None)
        return calls, events, runner

    def test_volc_success_zero_fallback_calls(self):
        calls, events, _ = self._run(VolcFake())
        self.assertEqual(0, calls["stt"], "火山成功不得调用备用")
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))
        self.assertEqual("你好，这是火山识别", tr[0]["text"])

    def test_volc_finish_error_one_fallback(self):
        calls, events, _ = self._run(VolcFake(fail_code="auth_error", fail_at="finish"))
        self.assertEqual(1, calls["stt"], "可降级失败必须恰好一次备用")
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))
        self.assertEqual("备用识别结果", tr[0]["text"])

    def test_volc_start_error_one_fallback(self):
        calls, events, _ = self._run(VolcFake(fail_code="missing_credentials",
                                           fail_at="start"))
        self.assertEqual(1, calls["stt"])
        tr = [f for t, f in events if t == "transcript"]
        self.assertEqual(1, len(tr))

    def test_transcript_exactly_once_all_paths(self):
        for volc in (VolcFake(), VolcFake(fail_code="auth_error", fail_at="finish"),
                     VolcFake(fail_code="missing_credentials", fail_at="start")):
            _, events, _ = self._run(volc)
            self.assertEqual(1, len([t for t, _ in events if t == "transcript"]))

    def test_metrics_no_contradiction(self):
        # 火山成功: PRIMARY_RESULT=success 且无 fallback reason (§3.1 一致性)
        _, _, runner = self._run(VolcFake())
        m = runner._turn_metrics
        self.assertEqual("success", m.get("ASR_PRIMARY_RESULT"))
        self.assertEqual("no", m.get("ASR_FALLBACK_USED"))
        self.assertEqual("none", m.get("ASR_FALLBACK_REASON"))
        self.assertEqual("volcengine_streaming", m.get("ASR_PROVIDER_USED"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
