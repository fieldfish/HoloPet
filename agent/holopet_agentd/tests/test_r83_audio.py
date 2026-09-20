"""test_r83_audio.py — R8_R3 真实录音/播放适配合同测试 (§7.1/§7.2/§7.5)。

全部使用注入式假进程: 不访问麦克风、声卡、公网。
覆盖: 启动/停止/超时/取消/设备失败/空音频/超长音频/最大字节门;
      参数数组无 shell 注入; 临时文件权限与精确清理; 播放成功/取消/失败/无残留。
"""

import io
import os
import struct
import sys
import tempfile
import threading
import unittest
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.audio.capture import (  # noqa: E402
    AudioCapture, AudioCaptureConfig, REASON_CONFIG, REASON_EMPTY,
    REASON_TOO_LARGE, REASON_PROCESS,
)
from holopet_agentd.audio.player import (  # noqa: E402
    AudioPlayer, REASON_CANCELLED, REASON_EMPTY as P_EMPTY,
    REASON_PROCESS as P_PROCESS, REASON_UNSUPPORTED,
)


def make_wav(seconds=1.0, rate=16000, amp=8000):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        n = int(seconds * rate)
        w.writeframes(struct.pack("<%dh" % n, *([amp] * n)))
    return buf.getvalue()


class FakeProc:
    """假进程: auto_exit_after 次 poll 后自行退出 (None = 永不自行退出)。"""

    def __init__(self, wav_path="", exit_code=0, term_exit=0, write_on_term=True,
                 argv=None, auto_exit_after=3):
        self.rc = None
        self._exit = exit_code
        self._term = term_exit
        self._wav = wav_path
        self._write = write_on_term
        self.argv = list(argv or [])
        self.terminated = False
        self.killed = False
        self._auto = auto_exit_after
        self._polls = 0

    def poll(self):
        if self.rc is None and self._auto is not None:
            self._polls += 1
            if self._polls >= self._auto:
                self.rc = self._exit
        return self.rc

    def terminate(self):
        self.terminated = True
        if self._write and self._wav:
            with open(self._wav, "wb") as f:
                f.write(make_wav(1.0))
        self.rc = self._term

    def kill(self):
        self.killed = True
        self.rc = -9

    def wait(self, timeout=None):
        return self.rc if self.rc is not None else self._exit


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="r83cap_")
        self.spawned = []

    def spawn(self, argv, stdout_path):
        proc = FakeProc(wav_path=argv[-1], argv=argv)
        self.spawned.append(proc)
        return proc

    def cfg(self, **kw):
        base = {"provider": "alsa", "device": "plughw:CARD=Device,DEV=0",
                "sample_rate": 16000, "bit_depth": 16, "channels": 1,
                "max_record_seconds": 5, "max_audio_bytes": 1048576}
        base.update(kw)
        return base

    def _leftovers(self):
        return [p for p in os.listdir(self.tmp) if p.startswith("holopet_r83_")]

    def _cap(self, binary="arecord", **kw):
        cap = AudioCapture(self.cfg(**kw), spawn=self.spawn, tmpdir=self.tmp,
                           binary=binary)
        # 绕过 PATH 检查: 注入 spawn 时不需要真实二进制
        cap._binary = binary
        orig_which = __import__("shutil").which
        __import__("shutil").which = lambda name, *a, **k: "/usr/bin/%s" % name
        self.addCleanup(lambda: setattr(__import__("shutil"), "which", orig_which))
        return cap

    def test_config_rejects_bad_values(self):
        for bad in ({"device": ""}, {"sample_rate": 100}, {"bit_depth": 12},
                    {"channels": 3}, {"max_record_seconds": 0},
                    {"max_audio_bytes": 0}, {"provider": "pulse"}):
            cfg = AudioCaptureConfig.from_dict(self.cfg(**bad))
            self.assertIsNotNone(cfg.validate(), bad)

    def test_start_uses_param_array_no_shell(self):
        cap = self._cap()
        self.assertTrue(cap.start("rid1"))
        argv = self.spawned[0].argv
        self.assertIsInstance(argv, list)
        self.assertEqual(argv[0], "arecord")
        self.assertIn("-D", argv)
        self.assertIn("plughw:CARD=Device,DEV=0", argv)
        self.assertIn("S16_LE", argv)
        for bad in (";", "&&", "|", "$(", "`"):
            self.assertFalse(any(bad in a for a in argv), argv)

    def test_stop_returns_audio_and_cleans(self):
        cap = self._cap()
        self.assertTrue(cap.start("rid"))
        res = cap.stop()
        self.assertIsNone(res.error)
        self.assertGreater(res.pcm_bytes, 0)
        self.assertGreater(res.duration_ms, 900)
        self.assertEqual("plughw", res.device_class)
        self.assertTrue(res.cleaned)
        self.assertEqual([], self._leftovers())
        self.assertFalse(cap.started)

    def test_stop_empty_audio_is_error(self):
        cap = self._cap()
        cap._spawn = lambda argv, log: FakeProc(wav_path=argv[-1], write_on_term=False)
        self.assertTrue(cap.start("rid"))
        res = cap.stop()
        self.assertIsNotNone(res.error)
        self.assertEqual(REASON_EMPTY, res.error.code)
        self.assertEqual([], self._leftovers())

    def test_stop_oversize_is_error(self):
        cap = self._cap(max_audio_bytes=1000)
        self.assertTrue(cap.start("rid"))
        res = cap.stop()
        self.assertIsNotNone(res.error)
        self.assertEqual(REASON_TOO_LARGE, res.error.code)

    def test_cancel_terminates_and_cleans(self):
        cap = self._cap()
        self.assertTrue(cap.start("rid"))
        cap.cancel()
        self.assertTrue(self.spawned[0].terminated)
        self.assertFalse(cap.started)
        self.assertEqual([], self._leftovers())

    def test_read_incremental_returns_bare_pcm(self):
        """R8_R3_R3_R2: 增量读必须跳过 WAV 头, 返回裸 PCM (火山流式只收 PCM)。"""
        cap = self._cap()
        self.assertTrue(cap.start("rid"))
        proc = self.spawned[0]
        with open(proc._wav, "wb") as f:
            f.write(make_wav(0.5))          # 0.5s WAV: 44B 头 + 16000B PCM
        chunk = cap.read_incremental()
        self.assertEqual(16000, len(chunk))
        self.assertEqual(chunk[:2], struct.pack("<h", 8000))
        chunk2 = cap.read_incremental()
        self.assertEqual(b"", chunk2, "无新增时返回空")
        # 追加一段后只返回新增裸 PCM
        with open(proc._wav, "ab") as f:
            f.write(struct.pack("<8000h", *([9000] * 8000)))
        chunk3 = cap.read_incremental()
        self.assertEqual(16000, len(chunk3))
        self.assertEqual(chunk3[:2], struct.pack("<h", 9000))
        cap.cancel()

    def test_process_failure_maps_stable_reason(self):
        cap = self._cap()
        cap._spawn = lambda argv, log: FakeProc(wav_path=argv[-1], term_exit=1,
                                                write_on_term=False)
        cap.start("rid")
        res = cap.stop()
        self.assertEqual(REASON_PROCESS, res.error.code)
        self.assertEqual(1, res.exit_code)

    def test_start_rejected_without_binary(self):
        cap = self._cap(binary="definitely_missing_bin")
        __import__("shutil").which = lambda name, *a, **k: None
        self.assertFalse(cap.start("rid"))

    def test_cancel_before_start_is_safe(self):
        cap = self._cap()
        cap.cancel()
        self.assertFalse(cap.started)
        self.assertIsNotNone((cap.stop()).error)


class PlayerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="r83play_")
        self.spawned = []
        self.seen_modes = []

    def spawn(self, argv, stdout_path):
        proc = FakeProc(argv=argv)
        self.spawned.append(proc)
        return proc

    def _player(self, binary="aplay", **kw):
        cfg = {"provider": "alsa", "device": "plughw:CARD=Device,DEV=0"}
        cfg.update(kw)
        p = AudioPlayer(cfg, spawn=self.spawn, tmpdir=self.tmp, binary=binary)
        self.seen_modes.append(None)
        return p

    def _which(self, value="/usr/bin/aplay"):
        """名字感知的 which 替身: 只让 aplay 存在 (ffplay 等解码器不存在)。"""
        orig = __import__("shutil").which

        def fake(name, *a, **k):
            return value if name == "aplay" else None

        __import__("shutil").which = fake
        self.addCleanup(lambda: setattr(__import__("shutil"), "which", orig))
        return orig

    def _leftovers(self):
        return [p for p in os.listdir(self.tmp) if p.startswith("holopet_r83_play_")]

    def test_play_wav_success_and_cleanup(self):
        self._which("/usr/bin/aplay")
        p = self._player()
        seen = {"files": None, "mode": None}

        def on_started():
            # 播放中: 临时文件必须存在 (POSIX 下还须 0600)
            files = self._leftovers()
            seen["files"] = list(files)
            if files:
                seen["mode"] = os.stat(
                    os.path.join(self.tmp, files[0])).st_mode & 0o777
        res = p.play("rid", make_wav(0.5), "audio/wav", on_started=on_started)
        self.assertTrue(res.played, res)
        self.assertEqual(0, res.exit_code)
        self.assertEqual(1, len(seen["files"] or []), "播放期间临时文件必须存在")
        if os.name == "posix":
            self.assertEqual(0o600, seen["mode"])
        self.assertEqual([], self._leftovers())

    def test_cancel_stops_playback(self):
        self._which("/usr/bin/aplay")
        p = self._player()
        cancel = threading.Event()

        def spawn_slow(argv, log):
            proc = FakeProc(argv=argv, auto_exit_after=None)
            self.spawned.append(proc)
            # 模拟仍在播放: poll() 返回 None; terminate() 置 rc
            return proc

        p._spawn = spawn_slow
        cancel.set()
        res = p.play("rid", make_wav(1.0), "audio/wav", cancel_event=cancel)
        self.assertFalse(res.played)
        self.assertEqual(REASON_CANCELLED, res.error_code)
        self.assertTrue(self.spawned[0].terminated)
        self.assertEqual([], self._leftovers())

    def test_player_exit_nonzero_maps_error(self):
        self._which("/usr/bin/aplay")
        p = self._player()
        p._spawn = lambda argv, log: FakeProc(argv=argv, exit_code=2)
        res = p.play("rid", make_wav(0.2), "audio/wav")
        self.assertFalse(res.played)
        self.assertEqual(P_PROCESS, res.error_code)
        self.assertEqual(2, res.exit_code)
        self.assertEqual([], self._leftovers())

    def test_empty_audio_and_bad_mime(self):
        self._which("/usr/bin/aplay")
        p = self._player()
        self.assertEqual(P_EMPTY, p.play("rid", b"", "audio/wav").error_code)
        self.assertEqual(REASON_UNSUPPORTED,
                         p.play("rid", b"xx", "application/octet-stream").error_code)
        self.assertEqual(REASON_UNSUPPORTED,
                         p.play("rid", b"xx", "audio/mpeg").error_code)

    def test_missing_binary_device_reason(self):
        p = self._player(binary="missing_player_bin")
        __import__("shutil").which = lambda name, *a, **k: None
        res = p.play("rid", make_wav(0.2), "audio/wav")
        self.assertFalse(res.played)
        self.assertIn(res.error_code, ("playback_device_missing",
                                       "playback_process_error"))

    def test_bad_config_rejected(self):
        p = AudioPlayer({"provider": "alsa", "device": ""}, spawn=self.spawn,
                        tmpdir=self.tmp)
        res = p.play("rid", make_wav(0.2), "audio/wav")
        self.assertEqual("playback_config_error", res.error_code)


if __name__ == "__main__":
    unittest.main(verbosity=2)
