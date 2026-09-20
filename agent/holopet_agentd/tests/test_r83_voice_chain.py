"""test_r83_voice_chain.py — R8_R3 真人语音链替身集成 (§7.6/§7.7)。

capture → ASR → LLM → display/expression → TTS → playback 全链, 全部本地替身:
不访问麦克风、声卡、公网。text_only=true 必须为零 ASR/TTS/播放副作用。
"""

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
from holopet_agentd.providers.stt import OpenAiCompatSttProvider  # noqa: E402
from holopet_agentd.providers.tts import OpenAiCompatTtsProvider  # noqa: E402

ASR_ENV = "HOLOPET_R83_CHAIN_ASR_KEY"
TTS_ENV = "HOLOPET_R83_CHAIN_TTS_KEY"
DUMMY = "placeholder-only"


def make_wav(seconds=0.4, amp=0):
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
        self.wav = wav
        self.started = 0
        self.stopped = 0
        self.cancelled = 0

    def config_reason(self):
        return None

    def start(self, rid):
        self.started += 1
        return True

    def stop(self):
        self.stopped += 1
        pcm = max(0, len(self.wav) - 44)
        return PcmResult(wav_bytes=self.wav, pcm_bytes=pcm, duration_ms=400,
                         device_class="plughw", exit_code=0, cleaned=True)

    def cancel(self):
        self.cancelled += 1


class FakePlayer:
    def __init__(self, fail=None, cancel=False):
        self.calls = []
        self.fail = fail
        self.cancel = cancel

    def play(self, rid, audio, mime, cancel_event=None, estimator=None,
             on_started=None):
        self.calls.append({"rid": rid, "audio": audio, "mime": mime})
        if on_started:
            on_started()
        if self.cancel:
            return PlaybackResult(error_code="playback_cancelled")
        if self.fail:
            return PlaybackResult(error_code=self.fail,
                                  error_message="injected")
        return PlaybackResult(played=True, duration_ms=400, exit_code=0,
                              cleaned=True)


class Resp:
    def __init__(self, body, ctype):
        self._b = body
        self.headers = {"Content-Type": ctype}

    def read(self, n=None):
        return self._b if n is None else self._b[:n]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def run_chain(config, capture=None, player=None, asr_text="你好，请介绍一下自己",
              tts_wav=None, tts_ctype="audio/wav"):
    """跑一轮完整替身链; 返回 (events, capture, player)。"""
    events = []
    cap = capture or FakeCapture(wav=make_wav(0.4, amp=2000))
    ply = player or FakePlayer()
    tts_audio = tts_wav if tts_wav is not None else make_wav(0.4, amp=0)

    orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
            worker_main.make_stt_provider, worker_main.make_tts_provider)
    worker_main.make_audio_capture = lambda cfg: cap
    worker_main.make_audio_player = lambda cfg: ply
    worker_main.make_stt_provider = lambda cfg: OpenAiCompatSttProvider(
        cfg, opener=lambda req, timeout=None: Resp(
            json.dumps({"text": asr_text}).encode(), "application/json"))
    worker_main.make_tts_provider = lambda cfg: OpenAiCompatTtsProvider(
        cfg, opener=lambda req, timeout=None: Resp(tts_audio, tts_ctype))
    try:
        os.environ[ASR_ENV] = DUMMY
        os.environ[TTS_ENV] = DUMMY
        runner = worker_main.TurnRunner(config)
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        runner.run("t1", threading.Event(),
                   lambda r, t, **f: (events.append((t, f)), True)[1])
    finally:
        worker_main.make_audio_capture, worker_main.make_audio_player, \
            worker_main.make_stt_provider, worker_main.make_tts_provider = orig
        os.environ.pop(ASR_ENV, None)
        os.environ.pop(TTS_ENV, None)
    return events, cap, ply


def voice_config(**over):
    cfg = {
        "provider": "fake", "text_only": False,
        "audio_capture": {"provider": "alsa", "device": "plughw:CARD=X,DEV=0",
                          "max_record_seconds": 3, "max_audio_bytes": 1048576},
        "audio_playback": {"provider": "alsa", "device": "plughw:CARD=X,DEV=0"},
        "asr": {"provider": "openai_compatible",
                "base_url": "https://asr.invalid/v1", "model": "asr-x",
                "api_key_env_name": ASR_ENV},
        "tts": {"provider": "openai_compatible",
                "base_url": "https://tts.invalid/v1", "model": "tts-x",
                "voice": "v", "api_key_env_name": TTS_ENV},
        "response_policy": {"short_max_chars": 60},
    }
    cfg.update(over)
    return cfg


class VoiceChainTests(unittest.TestCase):
    def test_full_chain_order_and_real_audio(self):
        events, cap, ply = run_chain(voice_config())
        kinds = [t for t, _ in events]
        self.assertEqual("listening", events[0][1]["value"])
        self.assertIn("transcript", kinds)
        tr = [f for t, f in events if t == "transcript"][0]
        self.assertEqual("你好，请介绍一下自己", tr["text"])
        self.assertIn("thinking", [f.get("value") for t, f in events if t == "state"])
        self.assertIn("speaking", [f.get("value") for t, f in events if t == "state"])
        self.assertEqual("idle", [f.get("value") for t, f in events
                                  if t == "state"][-1])
        self.assertEqual("done", kinds[-1])
        # 顺序合同: transcript → content → response_complete → expression
        # → tts_started → tts_level → tts_finished → done
        order = [k for k in kinds if k in ("transcript", "content",
                                           "response_complete", "expression",
                                           "tts_started", "tts_level",
                                           "tts_finished", "done")]
        self.assertEqual(sorted(order, key=lambda k: order.index(k)), order)
        for a, b in (("transcript", "content"), ("content", "response_complete"),
                     ("response_complete", "expression"),
                     ("expression", "tts_started"), ("tts_started", "tts_level"),
                     ("tts_level", "tts_finished"), ("tts_finished", "done")):
            self.assertLess(order.index(a), order.index(b), (a, b, order))
        # 采集: start/stop 各一次; 播放器收到真实音频 (静音 → 0 RMS 级别)
        self.assertEqual(1, cap.started)
        self.assertEqual(1, cap.stopped)
        self.assertEqual(1, len(ply.calls))
        self.assertEqual("audio/wav", ply.calls[0]["mime"])
        levels = [f["rms"] for t, f in events if t == "tts_level"]
        self.assertTrue(levels)
        self.assertEqual([0.0] * len(levels), levels,
                         "tts_level 必须来自真实 PCM (静音=0), 不是 Fake 固定波形")
        self.assertNotIn(DUMMY, json.dumps(events, ensure_ascii=False))

    def test_capture_failure_goes_idle_with_stable_reason(self):
        class BadCapture(FakeCapture):
            def start(self, rid):
                return False

            def config_reason(self):
                return "capture_device_missing"
        events, cap, ply = run_chain(voice_config(), capture=BadCapture())
        self.assertEqual([], ply.calls)
        errs = [f for t, f in events if t == "error"]
        self.assertTrue(errs)
        self.assertEqual("stt_error", errs[0]["code"])
        self.assertEqual("stt:capture_device_missing", errs[0]["text"])
        self.assertEqual("idle", [f.get("value") for t, f in events
                                  if t == "state"][-1])

    def test_playback_failure_no_tts_finished(self):
        events, cap, ply = run_chain(voice_config(),
                                     player=FakePlayer(fail="playback_process_error"))
        kinds = [t for t, _ in events]
        self.assertIn("tts_started", kinds)   # 已开始播放才报错
        self.assertNotIn("tts_finished", kinds)
        errs = [f for t, f in events if t == "error"]
        self.assertEqual("tts_error", errs[0]["code"])
        self.assertEqual("tts:playback_process_error", errs[0]["text"])

    def test_playback_cancel_no_tts_finished(self):
        events, cap, ply = run_chain(voice_config(), player=FakePlayer(cancel=True))
        kinds = [t for t, _ in events]
        self.assertNotIn("tts_finished", kinds)

    def test_text_only_zero_audio_side_effects(self):
        calls = {"cap": 0, "ply": 0, "stt": 0, "tts": 0}
        orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
                worker_main.make_stt_provider, worker_main.make_tts_provider)

        def boom(kind):
            def _f(cfg):
                calls[kind] += 1
                raise AssertionError("text_only 不得构造 " + kind)
            return _f

        worker_main.make_audio_capture = boom("cap")
        worker_main.make_audio_player = boom("ply")
        worker_main.make_stt_provider = boom("stt")
        worker_main.make_tts_provider = boom("tts")
        try:
            cfg = voice_config(text_only=True, stt_inject={"t1": "文字路径"})
            runner = worker_main.TurnRunner(cfg)
            runner._audio_stop = threading.Event()
            runner._audio_stop.set()
            events = []
            runner.run("t1", threading.Event(),
                       lambda r, t, **f: (events.append((t, f)), True)[1])
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player, \
                worker_main.make_stt_provider, worker_main.make_tts_provider = orig
        kinds = [t for t, _ in events]
        self.assertEqual({"cap": 0, "ply": 0, "stt": 0, "tts": 0}, calls)
        for k in ("tts_started", "tts_level", "tts_finished"):
            self.assertNotIn(k, kinds)
        self.assertIn("done", kinds)

    def test_asr_error_maps_to_stt_error_event(self):
        orig = (worker_main.make_audio_capture, worker_main.make_audio_player,
                worker_main.make_stt_provider, worker_main.make_tts_provider)
        worker_main.make_audio_capture = lambda cfg: FakeCapture(make_wav(0.3))
        worker_main.make_audio_player = lambda cfg: FakePlayer()
        worker_main.make_stt_provider = lambda cfg: OpenAiCompatSttProvider(
            cfg, opener=lambda req, timeout=None: Resp(b"not-json",
                                                       "application/json"))
        worker_main.make_tts_provider = lambda cfg: OpenAiCompatTtsProvider(
            cfg, opener=lambda req, timeout=None: Resp(make_wav(0.2),
                                                       "audio/wav"))
        try:
            os.environ[ASR_ENV] = DUMMY
            os.environ[TTS_ENV] = DUMMY
            cfg = voice_config()
            runner = worker_main.TurnRunner(cfg)
            runner._audio_stop = threading.Event()
            runner._audio_stop.set()
            events = []
            runner.run("t1", threading.Event(),
                       lambda r, t, **f: (events.append((t, f)), True)[1])
        finally:
            worker_main.make_audio_capture, worker_main.make_audio_player,                 worker_main.make_stt_provider, worker_main.make_tts_provider = orig
            os.environ.pop(ASR_ENV, None)
            os.environ.pop(TTS_ENV, None)
        errs = [f for t, f in events if t == "error"]
        self.assertEqual("stt_error", errs[0]["code"])
        self.assertEqual("stt:malformed_response", errs[0]["text"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
