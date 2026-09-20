"""audio/capture.py — R8_R3 真实录音适配 (ALSA/arecord, 可注入)。

接口职责：
  AudioCapture.start(request_id) -> bool
  AudioCapture.stop()            -> PcmResult     正常收尾 (含最长录音时限)
  AudioCapture.cancel()          -> None          取消/断连/SIGTERM: 终止进程 + 删临时文件

要求 (§6.1):
  - 参数数组调用 (禁止 shell 拼接), 不写用户原始语音到正式交付包;
  - 记录时长/字节数/设备类别/退出码; 空音频、设备不存在、权限错误、
    进程异常、超长音频映射稳定错误码;
  - 达到最长录音时间也必须有稳定收尾 (返回已录内容, 不是崩溃)。

安全: 临时文件 0600, 仅本地路径; 不记录设备详情以外的隐私信息。
"""

import logging
import os
import shutil
import subprocess
import tempfile
import threading
import time
import wave
from dataclasses import dataclass, field
from typing import Callable, Optional

LOG = logging.getLogger("holopet_agentd")

# 稳定公开错误原因 (事件 code=stt_error, text=stt:<reason>)
REASON_CONFIG = "capture_config_error"
REASON_DEVICE_MISSING = "capture_device_missing"
REASON_PERMISSION = "capture_permission_denied"
REASON_PROCESS = "capture_process_error"
REASON_EMPTY = "capture_empty_audio"
REASON_TOO_LARGE = "capture_audio_too_large"

_ALSA_FMT = {8: "U8", 16: "S16_LE", 24: "S24_3LE", 32: "S32_LE"}


@dataclass
class AudioCaptureConfig:
    provider: str = "alsa"
    device: str = "plughw:CARD=Device,DEV=0"
    sample_rate: int = 16000
    bit_depth: int = 16
    channels: int = 1
    max_record_seconds: int = 15
    max_audio_bytes: int = 1048576

    @classmethod
    def from_dict(cls, cfg: dict) -> "AudioCaptureConfig":
        cfg = cfg or {}
        return cls(
            provider=str(cfg.get("provider", "alsa")),
            device=str(cfg.get("device", "")),
            sample_rate=int(cfg.get("sample_rate", 16000)),
            bit_depth=int(cfg.get("bit_depth", 16)),
            channels=int(cfg.get("channels", 1)),
            max_record_seconds=int(cfg.get("max_record_seconds", 15)),
            max_audio_bytes=int(cfg.get("max_audio_bytes", 1048576)),
        )

    def validate(self) -> Optional[str]:
        if self.provider != "alsa":
            return REASON_CONFIG
        if not self.device:
            return REASON_CONFIG
        if not (8000 <= self.sample_rate <= 48000):
            return REASON_CONFIG
        if self.bit_depth not in _ALSA_FMT:
            return REASON_CONFIG
        if self.channels not in (1, 2):
            return REASON_CONFIG
        if not (1 <= self.max_record_seconds <= 300):
            return REASON_CONFIG
        if self.max_audio_bytes <= 0:
            return REASON_CONFIG
        return None

    def device_class(self) -> str:
        return self.device.split(":", 1)[0] if self.device else ""


@dataclass
class AudioError:
    code: str
    message: str


@dataclass
class PcmResult:
    wav_bytes: bytes = b""
    pcm_bytes: int = 0
    duration_ms: int = 0
    device_class: str = ""
    exit_code: Optional[int] = None
    error: Optional[AudioError] = None
    cleaned: bool = False


def _default_spawn(argv: list, stdout_path: str):
    """生产实现: 参数数组 + 无 shell; stdout/stderr 落临时文件 (0600)。"""
    out = open(stdout_path, "wb")
    try:
        return subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT,
                                shell=False, stdin=subprocess.DEVNULL)
    finally:
        out.close()


class AudioCapture:
    """单轮录音: start → (外部 stop_recording) → stop/cancel。"""

    def __init__(self, cfg: dict, spawn: Optional[Callable] = None,
                 tmpdir: Optional[str] = None, binary: str = "arecord"):
        self._cfg = AudioCaptureConfig.from_dict(cfg)
        self._spawn = spawn or _default_spawn
        self._binary = binary
        self._tmpdir = tmpdir or tempfile.gettempdir()
        self._lock = threading.Lock()
        self._proc = None
        self._wav_path = ""
        self._log_path = ""
        self._started_at = 0.0
        self._request_id = ""
        self._read_off = 0            # read_incremental 读游标 (裸 PCM)
        self._data_off = None        # WAV data 区偏移 (首次探测后缓存)

    # ---- 状态 ----
    @property
    def started(self) -> bool:
        with self._lock:
            return self._proc is not None

    def config_reason(self) -> Optional[str]:
        return self._cfg.validate()

    @property
    def wav_path(self) -> str:
        return self._wav_path

    def read_incremental(self):
        """流式: 返回自上次调用以来新写入的**裸 PCM** (跳过 WAV 头)。

        R8_R3_R3_R2: 火山流式 ASR 只接受裸 PCM; 之前直接返回 WAV 容器
        字节 (含 RIFF 头), 服务端把头部当噪声 → 0 partial/empty。
        """
        if not self._wav_path or not os.path.exists(self._wav_path):
            return b""
        try:
            size = os.path.getsize(self._wav_path)
        except OSError:
            return b""
        if size <= self._read_off:
            return b""
        with open(self._wav_path, "rb") as f:
            if self._data_off is None:
                self._data_off = self._probe_data_offset(f, size)
            start = max(self._read_off, self._data_off)
            if size <= start:
                self._read_off = size
                return b""
            f.seek(start)
            chunk = f.read(size - start)
        self._read_off = size
        return chunk

    @staticmethod
    def _probe_data_offset(f, size):
        """有界扫描 RIFF/WAVE 头, 返回 data chunk 负载偏移 (异常→0)。"""
        try:
            head = f.read(min(size, 4096))
        except OSError:
            return 0
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            return 0
        off = 12
        while off + 8 <= len(head):
            cid = head[off:off + 4]
            csz = int.from_bytes(head[off + 4:off + 8], "little")
            if cid == b"data":
                return off + 8
            off += 8 + csz + (csz & 1)
        return 0

    # ---- 生命周期 ----
    def start(self, request_id: str) -> bool:
        reason = self._cfg.validate()
        if reason:
            LOG.warning("capture start rejected: %s", reason)
            return False
        if not shutil.which(self._binary):
            LOG.warning("capture binary missing: %s", self._binary)
            return False
        with self._lock:
            if self._proc is not None:
                return False
            self._request_id = request_id
            base = os.path.join(self._tmpdir, "holopet_r83_%s_" % os.getpid())
            fd, self._wav_path = tempfile.mkstemp(prefix=os.path.basename(base),
                                                  suffix=".wav",
                                                  dir=self._tmpdir)
            os.close(fd)
            os.chmod(self._wav_path, 0o600)
            self._log_path = self._wav_path + ".log"
            argv = [
                self._binary, "-q",
                "-D", self._cfg.device,
                "-f", _ALSA_FMT[self._cfg.bit_depth],
                "-r", str(self._cfg.sample_rate),
                "-c", str(self._cfg.channels),
                "-d", str(self._cfg.max_record_seconds),
                "-t", "wav",
                self._wav_path,
            ]
            try:
                self._proc = self._spawn(argv, self._log_path)
            except OSError as e:
                LOG.warning("capture spawn failed: %s", type(e).__name__)
                self._proc = None
                self._cleanup_locked()
                return False
            self._started_at = time.monotonic()
            self._read_off = 0
            LOG.info("capture started rid=%s dev_class=%s rate=%d bits=%d ch=%d",
                     request_id, self._cfg.device_class(), self._cfg.sample_rate,
                     self._cfg.bit_depth, self._cfg.channels)
            return True

    def stop(self) -> PcmResult:
        """正常收尾: 终止采集进程并返回已录音频 (含最长录音时限路径)。"""
        with self._lock:
            proc = self._proc
            wav_path = self._wav_path
            started = self._started_at
        if proc is None:
            return PcmResult(error=AudioError(REASON_PROCESS, "not started"))
        rc = self._terminate(proc)
        elapsed_ms = int(max(0.0, time.monotonic() - started) * 1000) if started else 0
        try:
            with open(wav_path, "rb") as f:
                wav_bytes = f.read()
        except OSError:
            wav_bytes = b""
        pcm_bytes, wav_duration_ms = self._pcm_stats(wav_bytes)
        with self._lock:
            self._proc = None
            self._cleanup_locked()
        self._read_off = 0
        self._data_off = None
        res = PcmResult(wav_bytes=wav_bytes, pcm_bytes=pcm_bytes,
                        duration_ms=wav_duration_ms or elapsed_ms,
                        device_class=self._cfg.device_class(),
                        exit_code=rc, cleaned=True)
        if rc not in (0, None) and pcm_bytes == 0:
            res.error = AudioError(self._map_exit(rc), "capture process failed")
            return res
        if pcm_bytes <= 0 or wav_duration_ms <= 0:
            res.error = AudioError(REASON_EMPTY, "empty capture")
            return res
        if pcm_bytes > self._cfg.max_audio_bytes:
            res.error = AudioError(REASON_TOO_LARGE,
                                   "%d > %d" % (pcm_bytes,
                                                self._cfg.max_audio_bytes))
        return res

    def cancel(self) -> None:
        """取消/断连/SIGTERM: 终止进程并清理临时文件, 不返回音频。"""
        with self._lock:
            proc = self._proc
            self._proc = None
        if proc is not None:
            self._terminate(proc)
        with self._lock:
            self._cleanup_locked()
        self._read_off = 0
        LOG.info("capture cancelled rid=%s", self._request_id)

    # ---- 内部 ----
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

    def _cleanup_locked(self) -> None:
        for p in (self._wav_path, self._log_path):
            if p and os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
        self._wav_path = ""
        self._log_path = ""
        self._started_at = 0.0

    def _map_exit(self, rc: int) -> str:
        # arecord 常见退出: 1 = 通用错误 (设备忙/权限/不存在, 由 stderr 区分,
        # 但 stderr 不进入日志正文) → 统一 process_error; 权限单独判定。
        if rc in (13,):
            return REASON_PERMISSION
        return REASON_PROCESS

    @staticmethod
    def _pcm_stats(wav_bytes: bytes):
        """返回 (pcm 字节数, 时长 ms); 非 WAV/空 → (0, 0)。"""
        if len(wav_bytes) < 44 or wav_bytes[:4] != b"RIFF":
            return (0, 0)
        try:
            import io
            with wave.open(io.BytesIO(wav_bytes), "rb") as w:
                frames = w.getnframes()
                rate = w.getframerate() or 0
                pcm = frames * w.getnchannels() * w.getsampwidth()
                dur_ms = int(frames * 1000 / rate) if rate else 0
                return (pcm, dur_ms)
        except Exception:
            return (0, 0)
