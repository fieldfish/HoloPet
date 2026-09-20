"""providers/tts.py — TTS Provider 合同  V6-R2

最小可交付接口:
  TtsProvider.synthesize(request_id, text, voice) -> AudioResult
第一版不接真实引擎: FakeTtsProvider 产出可验证的音频缓冲元数据
(started/level/finished 事件由 worker 按 AudioResult 驱动, 不用 sleep 冒充)。
"""

import json
import logging
import os
import socket
import struct
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Optional

from .errors import (
    AuthError,
    MalformedResponseError,
    MissingApiKeyError,
    NetworkError,
    ProviderConfigError,
    RateLimitedError,
    ResponseTooLargeError,
    TimeoutError_,
)

LOG = logging.getLogger("holopet_agentd")


@dataclass
class AudioChunkMeta:
    index: int
    duration_ms: int
    rms: float = 0.5        # 0..1


@dataclass
class TtsError:
    code: str        # "empty_text" | "text_too_long" | "engine" | 稳定原因码
    message: str


@dataclass
class AudioResult:
    chunks: list = field(default_factory=list)   # [AudioChunkMeta]
    duration_ms: int = 0
    error: Optional[TtsError] = None
    # R8_R3: 真实可播放载荷 (Fake 仍为空, 保持文字路径零副作用)
    audio: bytes = b""
    mime: str = ""


class TtsProvider:
    """TTS 合同 (子类实现 synthesize)"""

    def synthesize(self, request_id: str, text: str, voice: str) -> AudioResult:
        raise NotImplementedError


class FakeTtsProvider(TtsProvider):
    """离线确定性 TTS: 按文本长度产出真实 chunk 元数据 (可验证, 非 sleep)。"""

    def __init__(self, max_text_chars: int = 200, ms_per_char: int = 60,
                 error_code: str = ""):
        self._max_text_chars = max_text_chars
        self._ms_per_char = ms_per_char
        # R4 (P3): 故障注入 — {"tts_inject": {"error_code": "engine"}}
        self._error_code = error_code

    def synthesize(self, request_id: str, text: str, voice: str) -> AudioResult:
        del request_id, voice
        if self._error_code:
            return AudioResult(error=TtsError(
                self._error_code, f"injected tts error {self._error_code}"))
        if not text:
            return AudioResult(error=TtsError("empty_text", "no text to speak"))
        if len(text) > self._max_text_chars:
            return AudioResult(error=TtsError(
                "text_too_long", f"{len(text)} > {self._max_text_chars}"))
        total_ms = max(200, len(text) * self._ms_per_char)
        # 每 ~1.2s 一个 chunk, 电平按确定性伪波形
        chunks = []
        chunk_ms = 1200
        idx = 0
        t = 0
        while t < total_ms:
            dur = min(chunk_ms, total_ms - t)
            rms = round(0.25 + 0.35 * ((idx % 3) * 0.5), 3)   # 0.25..0.6 确定性
            chunks.append(AudioChunkMeta(index=idx, duration_ms=dur, rms=rms))
            t += dur
            idx += 1
        return AudioResult(chunks=chunks, duration_ms=total_ms)


# ---------------------------------------------------------------------------
# R8_R3: 真实 TTS 适配 + 真实音频电平 (tts_level 来自实际 PCM, 非固定波形)
# ---------------------------------------------------------------------------

def wav_pcm_payload(data: bytes) -> bytes:
    """从 WAV 容器取 PCM 载荷; 非 WAV → 原样返回 (交由播放器判定)。"""
    if len(data) >= 44 and data[:4] == b"RIFF":
        return data[44:]
    return data


def rms_envelope(audio: bytes, mime: str, chunk_ms: int = 1200,
                 sample_rate: int = 16000, bits: int = 16) -> list:
    """由真实音频计算分块 RMS (0..1) — tts_level 的真实来源。

    16bit little-endian 单声道假设由调用方保证 (ASR/TTS 合同固定为该格式);
    数据不足一块时按剩余长度成块; 空音频返回空列表。
    """
    pcm = wav_pcm_payload(audio) if mime in ("audio/wav", "audio/x-wav",
                                             "audio/wave") else audio
    if bits != 16:
        return []
    bytes_per_ms = max(1, int(sample_rate * 2 / 1000))
    chunk_bytes = max(2, chunk_ms * bytes_per_ms)
    chunk_bytes -= chunk_bytes % 2
    out = []
    idx = 0
    for off in range(0, len(pcm), chunk_bytes):
        block = pcm[off:off + chunk_bytes]
        if len(block) < 2:
            break
        n = len(block) // 2
        try:
            samples = struct.unpack("<%dh" % n, block[:n * 2])
        except struct.error:
            break
        peak = max(1, max(abs(s) for s in samples))
        rms = (sum(float(s) * s for s in samples) / n) ** 0.5 / 32768.0
        dur = int(n * 1000 / sample_rate)
        out.append(AudioChunkMeta(index=idx, duration_ms=dur,
                                  rms=round(min(1.0, rms), 4)))
        idx += 1
        del peak
    return out


class OpenAiCompatTtsProvider(TtsProvider):
    """真实 TTS: POST {base_url}/audio/speech → 音频字节 (默认请求 wav)。

    稳定原因码复用 providers.errors 词汇; 空音频 → "empty_audio";
    不支持的响应 MIME → "malformed_response" (由播放层再判格式)。
    Key 只从 api_key_env_name 读取。
    """

    def __init__(self, config: dict, opener=None):
        self._config = config or {}
        self._base_url = (self._config.get("base_url") or "").rstrip("/")
        self._model = self._config.get("model") or "tts-1"
        self._voice = self._config.get("voice") or "alloy"
        self._api_key_env = (self._config.get("api_key_env_name")
                             or "HOLOPET_TTS_API_KEY")
        self._timeout = int(self._config.get("request_timeout_ms", 60000)) / 1000.0
        self._max_response_bytes = int(
            self._config.get("max_response_bytes", 4 * 1024 * 1024))
        self._response_format = self._config.get("response_format") or "wav"
        self._max_text_chars = int(self._config.get("max_text_chars", 400))
        self._opener = opener or urllib.request.urlopen

    @property
    def api_key_env_name(self) -> str:
        return self._api_key_env

    def key_present(self) -> bool:
        return bool(os.environ.get(self._api_key_env, ""))

    def synthesize(self, request_id: str, text: str, voice: str) -> AudioResult:
        del request_id
        if not text:
            return AudioResult(error=TtsError("empty_text", "no text to speak"))
        if len(text) > self._max_text_chars:
            return AudioResult(error=TtsError(
                "text_too_long", "%d > %d" % (len(text), self._max_text_chars)))
        try:
            audio, mime = self._request(text, voice or self._voice)
        except (ProviderConfigError, MissingApiKeyError, AuthError,
                RateLimitedError, TimeoutError_, ResponseTooLargeError,
                MalformedResponseError, NetworkError) as e:
            code = getattr(e, "code", "engine")
            return AudioResult(error=TtsError(code, code))
        except OSError as e:
            return AudioResult(error=TtsError("network_error",
                                              type(e).__name__))
        if not audio:
            return AudioResult(error=TtsError("empty_audio", "empty payload"))
        chunks = rms_envelope(audio, mime)
        duration_ms = sum(c.duration_ms for c in chunks)
        return AudioResult(chunks=chunks, duration_ms=duration_ms,
                           audio=audio, mime=mime)

    # ---- 内部 ----
    def _request(self, text: str, voice: str):
        if not self._base_url:
            raise ProviderConfigError("base_url missing")
        key = os.environ.get(self._api_key_env, "")
        if not key:
            raise MissingApiKeyError(self._api_key_env)
        payload = json.dumps({
            "model": self._model, "voice": voice, "input": text,
            "response_format": self._response_format,
        }).encode("utf-8")
        req = urllib.request.Request(
            self._base_url + "/audio/speech", data=payload, method="POST",
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + key})
        try:
            with self._opener(req, timeout=self._timeout) as resp:
                mime = (resp.headers.get("Content-Type", "")
                        if hasattr(resp, "headers") else "")
                raw = resp.read(self._max_response_bytes + 1)
        except urllib.error.HTTPError as e:
            code = e.code
            if code in (401, 403):
                raise AuthError("http %d" % code) from e
            if code == 429:
                raise RateLimitedError("http 429") from e
            if code >= 500:
                raise NetworkError("http %d" % code) from e
            raise MalformedResponseError("http %d" % code) from e
        except socket.timeout as e:
            raise TimeoutError_("read timeout") from e
        except urllib.error.URLError as e:
            if isinstance(getattr(e, "reason", None), socket.timeout):
                raise TimeoutError_("urlopen timeout") from e
            raise NetworkError("urlopen failed") from e
        except TimeoutError as e:
            raise TimeoutError_("timeout") from e
        if len(raw) > self._max_response_bytes:
            raise ResponseTooLargeError("response too large")
        mime = (mime or "").split(";")[0].strip().lower() or "audio/wav"
        if mime in ("application/json", "text/json", "text/plain"):
            raise MalformedResponseError("unexpected json payload")
        return raw, mime
