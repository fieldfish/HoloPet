"""providers/stt.py — STT Provider 合同  V6-R2

最小可交付接口:
  SttProvider.transcribe(request_id, pcm_audio, audio_format) -> TranscriptResult
PCM 边界必须带采样率、位深、声道、字节序与最大长度; 畸形或过大输入返回
结构化错误。第一版不接真实引擎: FakeSttProvider 支持测试夹具/demo 注入
的 transcript (按 request_id 保存, 作为 LLM 实际 user content)。
"""

import json
import logging
import os
import socket
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Dict, Optional

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
class AudioFormat:
    sample_rate: int = 16000      # Hz, 8000..48000
    bit_depth: int = 16           # 8/16/24/32
    channels: int = 1             # 1/2
    byte_order: str = "little"    # "little" | "big"
    max_len_bytes: int = 512 * 1024   # 最大 PCM 长度 (默认 512 KiB)


@dataclass
class SttError:
    code: str        # "bad_format" | "too_large" | "no_audio" | "no_transcript" | "engine"
    message: str


@dataclass
class TranscriptResult:
    text: str = ""
    confidence: float = 0.0       # 0..1
    error: Optional[SttError] = None


def validate_audio_format(fmt: AudioFormat) -> Optional[SttError]:
    if not (8000 <= fmt.sample_rate <= 48000):
        return SttError("bad_format", f"sample_rate {fmt.sample_rate} out of range")
    if fmt.bit_depth not in (8, 16, 24, 32):
        return SttError("bad_format", f"bit_depth {fmt.bit_depth} invalid")
    if fmt.channels not in (1, 2):
        return SttError("bad_format", f"channels {fmt.channels} invalid")
    if fmt.byte_order not in ("little", "big"):
        return SttError("bad_format", f"byte_order {fmt.byte_order} invalid")
    if fmt.max_len_bytes <= 0:
        return SttError("bad_format", "max_len_bytes must be positive")
    return None


class SttProvider:
    """STT 合同 (子类实现 transcribe)"""

    def transcribe(self, request_id: str, pcm_audio: bytes,
                   audio_format: AudioFormat) -> TranscriptResult:
        raise NotImplementedError


class FakeSttProvider(SttProvider):
    """离线确定性 STT: 只返回按 request_id 注入的 transcript。

    生产路径禁止固定文本; 注入只允许来自测试夹具或显式 demo 参数。
    """

    def __init__(self, inject: Optional[Dict[str, str]] = None):
        self._inject = dict(inject or {})   # request_id -> text

    def set_transcript(self, request_id: str, text: str) -> None:
        self._inject[request_id] = text

    def transcribe(self, request_id: str, pcm_audio: bytes,
                   audio_format: AudioFormat) -> TranscriptResult:
        err = validate_audio_format(audio_format)
        if err:
            return TranscriptResult(error=err)
        if not pcm_audio:
            return TranscriptResult(error=SttError("no_audio", "empty pcm buffer"))
        if len(pcm_audio) > audio_format.max_len_bytes:
            return TranscriptResult(error=SttError(
                "too_large", f"{len(pcm_audio)} > {audio_format.max_len_bytes}"))
        if request_id not in self._inject:
            return TranscriptResult(error=SttError(
                "no_transcript", f"no injected transcript for {request_id}"))
        return TranscriptResult(text=self._inject[request_id], confidence=1.0)


# ---------------------------------------------------------------------------
# R8_R3: 真实 ASR 适配 (OpenAI 兼容 /audio/transcriptions, multipart)
# ---------------------------------------------------------------------------

def _multipart(fields: Dict[str, str], filename: str,
               file_bytes: bytes) -> "tuple[bytes, str]":
    """构造 multipart/form-data 载荷 (仅标准库, 无第三方依赖)。"""
    boundary = "----holopetr83boundary0123456789"
    out = []
    for k, v in fields.items():
        out.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
                    % (boundary, k, v)).encode("utf-8"))
    out.append(("--%s\r\nContent-Disposition: form-data; name=\"file\"; "
                "filename=\"%s\"\r\nContent-Type: audio/wav\r\n\r\n"
                % (boundary, filename)).encode("utf-8"))
    out.append(file_bytes)
    out.append(("\r\n--%s--\r\n" % boundary).encode("utf-8"))
    return b"".join(out), "multipart/form-data; boundary=%s" % boundary


class OpenAiCompatSttProvider(SttProvider):
    """真实 ASR: POST {base_url}/audio/transcriptions (WAV 上传 → {"text": ...})。

    稳定原因码复用 providers.errors 词汇 (auth_error/rate_limited/timeout/
    network_error/response_too_large/malformed_response/config_error/
    missing_api_key); 空 transcript → "no_transcript"。
    Key 只从 api_key_env_name 指向的环境变量读取, 不记录、不回显。
    """

    def __init__(self, config: dict, opener=None):
        self._config = config or {}
        self._base_url = (self._config.get("base_url") or "").rstrip("/")
        self._model = self._config.get("model") or "whisper-1"
        self._api_key_env = (self._config.get("api_key_env_name")
                             or "HOLOPET_ASR_API_KEY")
        self._timeout = int(self._config.get("request_timeout_ms", 60000)) / 1000.0
        self._max_response_bytes = int(
            self._config.get("max_response_bytes", 256 * 1024))
        self._language = self._config.get("language") or ""
        self._opener = opener or urllib.request.urlopen

    # ---- 状态 ----
    @property
    def api_key_env_name(self) -> str:
        return self._api_key_env

    def key_present(self) -> bool:
        return bool(os.environ.get(self._api_key_env, ""))

    def transcribe(self, request_id: str, wav_audio: bytes,
                   audio_format: AudioFormat) -> TranscriptResult:
        del request_id
        err = validate_audio_format(audio_format)
        if err:
            return TranscriptResult(error=err)
        if not wav_audio:
            return TranscriptResult(error=SttError("no_audio", "empty wav buffer"))
        if len(wav_audio) > audio_format.max_len_bytes:
            return TranscriptResult(error=SttError(
                "too_large", "%d > %d" % (len(wav_audio),
                                          audio_format.max_len_bytes)))
        try:
            text = self._request(wav_audio, audio_format)
        except (ProviderConfigError, MissingApiKeyError) as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except AuthError as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except RateLimitedError as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except TimeoutError_ as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except ResponseTooLargeError as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except MalformedResponseError as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except NetworkError as e:
            return TranscriptResult(error=SttError(e.code, e.code))
        except OSError as e:
            return TranscriptResult(error=SttError("network_error",
                                                   type(e).__name__))
        if not text.strip():
            return TranscriptResult(error=SttError("no_transcript",
                                                   "empty transcript"))
        return TranscriptResult(text=text, confidence=1.0)

    # ---- 内部 ----
    def _request(self, wav_audio: bytes, fmt: AudioFormat) -> str:
        if not self._base_url:
            raise ProviderConfigError("base_url missing")
        key = os.environ.get(self._api_key_env, "")
        if not key:
            raise MissingApiKeyError(self._api_key_env)
        fields = {"model": self._model, "response_format": "json"}
        if self._language:
            fields["language"] = self._language
        body, content_type = _multipart(fields, "utterance.wav", wav_audio)
        req = urllib.request.Request(
            self._base_url + "/audio/transcriptions", data=body, method="POST",
            headers={"Content-Type": content_type,
                     "Authorization": "Bearer " + key})
        try:
            with self._opener(req, timeout=self._timeout) as resp:
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
        try:
            doc = json.loads(raw.decode("utf-8", errors="replace"))
        except ValueError as e:
            raise MalformedResponseError("json decode") from e
        if not isinstance(doc, dict):
            raise MalformedResponseError("payload not object")
        text = doc.get("text")
        if not isinstance(text, str):
            raise MalformedResponseError("text missing")
        return text
