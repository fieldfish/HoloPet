"""audio/player.py — R8_R3 真实播放适配 (ALSA/aplay, 可注入)。

接口职责：
  AudioPlayer.play(request_id, audio, mime, cancel_event) -> PlaybackResult

要求:
  - 参数数组调用 (禁止 shell), 临时音频文件 0600 且终态必删;
  - 取消/断连必须停止播放进程, 终态后不得再有 TTS 事件;
  - aplay 非零退出与设备缺失/权限错误映射稳定原因;
  - WAV 走 aplay; 其他 MIME 若无可用解码器 → playback_unsupported_format
    (不伪装成功)。

安全: 不把音频内容或设备细节写入日志正文。
"""

import logging
import os
import shutil
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

LOG = logging.getLogger("holopet_agentd")

REASON_CONFIG = "playback_config_error"
REASON_DEVICE_MISSING = "playback_device_missing"
REASON_PROCESS = "playback_process_error"
REASON_CANCELLED = "playback_cancelled"
REASON_EMPTY = "playback_empty_audio"
REASON_UNSUPPORTED = "playback_unsupported_format"

_MIME_EXT = {
    "audio/wav": ".wav", "audio/x-wav": ".wav", "audio/wave": ".wav",
    "audio/mpeg": ".mp3", "audio/mp3": ".mp3",
    "audio/ogg": ".ogg", "audio/opus": ".opus",
}


@dataclass
class AudioPlaybackConfig:
    provider: str = "alsa"
    device: str = "plughw:CARD=Device,DEV=0"

    @classmethod
    def from_dict(cls, cfg: dict) -> "AudioPlaybackConfig":
        cfg = cfg or {}
        return cls(provider=str(cfg.get("provider", "alsa")),
                   device=str(cfg.get("device", "")))

    def validate(self) -> Optional[str]:
        if self.provider != "alsa" or not self.device:
            return REASON_CONFIG
        return None


@dataclass
class PlaybackResult:
    played: bool = False
    duration_ms: int = 0
    device_class: str = ""
    exit_code: Optional[int] = None
    error_code: str = ""
    error_message: str = ""
    cleaned: bool = False


def _default_spawn(argv: list, stdout_path: str):
    out = open(stdout_path, "wb")
    try:
        return subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT,
                                shell=False, stdin=subprocess.DEVNULL)
    finally:
        out.close()


class AudioPlayer:
    """把音频载荷写到受控临时文件后由 aplay (参数数组) 播放。"""

    def __init__(self, cfg: dict, spawn: Optional[Callable] = None,
                 tmpdir: Optional[str] = None, binary: str = "aplay"):
        self._cfg = AudioPlaybackConfig.from_dict(cfg)
        self._spawn = spawn or _default_spawn
        self._binary = binary
        self._tmpdir = tmpdir or tempfile.gettempdir()
        self._lock = threading.Lock()
        self._proc = None

    @property
    def playing(self) -> bool:
        with self._lock:
            return self._proc is not None

    def config_reason(self) -> Optional[str]:
        return self._cfg.validate()

    def play(self, request_id: str, audio: bytes, mime: str,
             cancel_event=None, estimator=None, on_started=None) -> PlaybackResult:
        """播放一次音频; estimator(audio)->duration_ms 由调用方给出真实估算。

        on_started: 播放进程真正启动后回调 (TTS 合同要求 tts_started 只能在
        实际开始播放后发送)。
        """
        reason = self._cfg.validate()
        if reason:
            return PlaybackResult(error_code=reason, error_message="bad config")
        if not audio:
            return PlaybackResult(error_code=REASON_EMPTY,
                                  error_message="empty audio payload")
        ext = _MIME_EXT.get((mime or "").split(";")[0].strip().lower(), "")
        if not ext:
            return PlaybackResult(error_code=REASON_UNSUPPORTED,
                                  error_message="unsupported mime")
        if ext != ".wav":
            # 仅 WAV 由 aplay 原生解码; 其他格式需外部解码器 (未安装即明确失败)
            if not shutil.which("ffplay"):
                return PlaybackResult(error_code=REASON_UNSUPPORTED,
                                      error_message="no decoder for %s" % ext)
        if not shutil.which(self._binary):
            return PlaybackResult(error_code=REASON_DEVICE_MISSING,
                                  error_message="player binary missing")
        fd, path = tempfile.mkstemp(prefix="holopet_r83_play_", suffix=ext,
                                    dir=self._tmpdir)
        os.close(fd)
        os.chmod(path, 0o600)
        log_path = path + ".log"
        duration_ms = int(estimator(audio)) if estimator else 0
        argv = [self._binary, "-q", "-D", self._cfg.device, path]
        try:
            with open(path, "wb") as f:
                f.write(audio)
            proc = self._spawn(argv, log_path)
        except OSError as e:
            self._remove(path, log_path)
            return PlaybackResult(error_code=REASON_PROCESS,
                                  error_message=type(e).__name__)
        with self._lock:
            self._proc = proc
        if on_started is not None:
            try:
                on_started()
            except Exception:
                pass
        started = time.monotonic()
        cancelled = False
        rc: Optional[int] = None
        while True:
            if cancel_event is not None and cancel_event.is_set():
                cancelled = True
                rc = self._terminate(proc)
                break
            rc = proc.poll()
            if rc is not None:
                break
            if time.monotonic() - started > 120.0:      # 播放下限保护
                rc = self._terminate(proc)
                break
            time.sleep(0.02)
        with self._lock:
            self._proc = None
        self._remove(path, log_path)
        played_ms = int(max(0.0, time.monotonic() - started) * 1000)
        if cancelled:
            LOG.info("playback done rid=%s played=0 cancelled=1 exit=%s",
                     request_id, rc)
            return PlaybackResult(played=False, duration_ms=played_ms,
                                  exit_code=rc, error_code=REASON_CANCELLED,
                                  error_message="cancelled", cleaned=True,
                                  device_class=self._cfg.device.split(":", 1)[0])
        if rc != 0:
            LOG.info("playback done rid=%s played=0 exit=%s", request_id, rc)
            return PlaybackResult(played=False, duration_ms=played_ms,
                                  exit_code=rc, error_code=REASON_PROCESS,
                                  error_message="player exit != 0", cleaned=True,
                                  device_class=self._cfg.device.split(":", 1)[0])
        LOG.info("playback done rid=%s played=1 exit=0 duration_ms=%d",
                 request_id, duration_ms or played_ms)
        return PlaybackResult(played=True, duration_ms=duration_ms or played_ms,
                              exit_code=0, cleaned=True,
                              device_class=self._cfg.device.split(":", 1)[0])

    def cancel(self) -> None:
        with self._lock:
            proc = self._proc
            self._proc = None
        if proc is not None:
            self._terminate(proc)

    def _terminate(self, proc) -> Optional[int]:
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            return proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
            try:
                return proc.wait(timeout=5)
            except Exception:
                return None

    @staticmethod
    def _remove(*paths) -> None:
        for p in paths:
            if p and os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
