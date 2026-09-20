"""providers/openai_compat.py — OpenAI Compatible Provider  V6-R2 (R8_R2 改造)

接口: chat(messages) -> {"content","tool_calls","finish_reason"}
只负责 HTTP POST; 工具结果回灌由 worker turn runner 负责。
API Key 只从环境变量读取; 仅标准库 (urllib)。

R8_R2 (工作包 B):
  - 全部失败路径映射为 providers.errors 的稳定类别 (config_error /
    missing_api_key / auth_error / rate_limited / timeout / network_error /
    response_too_large / malformed_response), 不再统一 RuntimeError;
  - HTTP 状态、响应大小、JSON 类型、choices/message/content/tool_calls
    结构逐项校验;
  - 日志只允许 api_key_present=yes/no 与类别名; 不记录 Key、Authorization、
    响应正文或 URL 查询串。
"""

import json
import logging
import os
import socket
import urllib.error
import urllib.request

from ..tools import TOOL_REGISTRY
from .errors import (
    AuthError,
    MalformedResponseError,
    MissingApiKeyError,
    ModelBlockedError,
    NetworkError,
    ProviderConfigError,
    RateLimitedError,
    ResponseTooLargeError,
    TimeoutError_,
)

LOG = logging.getLogger("holopet_agentd")


class OpenAICompatibleProvider:
    def __init__(self, config: dict):
        self._config = config
        self._base_url = (config.get("base_url") or "").rstrip("/")
        self._model = config.get("model") or "holopet-default"
        self._api_key_env = config.get("api_key_env_name") or "HOLOPET_API_KEY"
        # R4-R1 (9.4): connect_timeout_ms 无法与 read timeout 分离 —
        # 从活动配置移除 (总超时只有 request_timeout_ms); 文档标注 RESERVED
        self._request_timeout = int(config.get("request_timeout_ms", 30000)) / 1000.0
        self._max_tokens = int(config.get("max_output_tokens", 256))
        self._max_response_bytes = int(config.get("max_response_bytes",
                                                  256 * 1024))
        self._max_tool_rounds = int(config.get("max_tool_rounds", 4))

    def reset(self) -> None:
        pass

    def tool_schemas(self) -> list:
        return TOOL_REGISTRY

    def _api_key(self) -> str:
        key = os.environ.get(self._api_key_env, "")
        if not key:
            LOG.warning("provider config: api_key_present=no (env=%s)",
                        self._api_key_env)
            raise MissingApiKeyError(f"env {self._api_key_env} empty")
        LOG.info("provider config: api_key_present=yes")
        return key

    def chat(self, messages: list) -> dict:
        """单次 chat/completions 调用; 失败抛稳定 ProviderError 子类。"""
        if not self._base_url:
            raise ProviderConfigError("base_url missing")
        payload = {
            "model": self._model,
            "messages": messages,
            "max_tokens": self._max_tokens,
        }
        if self.tool_schemas():
            payload["tools"] = self.tool_schemas()
        url = f"{self._base_url}/chat/completions"
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url, data=data, method="POST",
            headers={"Content-Type": "application/json",
                     "Authorization": f"Bearer {self._api_key()}"})
        try:
            with urllib.request.urlopen(req, timeout=self._request_timeout) as resp:
                raw = resp.read(self._max_response_bytes + 1)
        except urllib.error.HTTPError as e:
            status = int(getattr(e, "code", 0) or 0)
            if status in (401, 403):
                raise AuthError(f"http {status}") from e
            if status == 429:
                raise RateLimitedError("http 429") from e
            if status >= 500:
                raise NetworkError(f"http {status}") from e
            # R8_R4_R3_R4_R5_R3 (B): 4xx 且错误体点名 model → 模型被拒,
            # 稳定上报 PROVIDER_MODEL_BLOCKED; 只检查类别词, 不落日志正文。
            if status in (400, 404):
                try:
                    snippet = (e.read(2048) or b"").decode(
                        "utf-8", "replace").lower()
                except OSError:
                    snippet = ""
                if "model" in snippet:
                    raise ModelBlockedError(f"http {status}") from e
            raise ProviderConfigError(f"http {status}") from e
        except urllib.error.URLError as e:
            reason = getattr(e, "reason", None)
            if isinstance(reason, (socket.timeout, TimeoutError)):
                raise TimeoutError_("urlopen timeout") from e
            raise NetworkError(type(e).__name__) from e
        except (socket.timeout, TimeoutError) as e:
            raise TimeoutError_("read timeout") from e
        except OSError as e:
            raise NetworkError(type(e).__name__) from e

        if len(raw) > self._max_response_bytes:
            raise ResponseTooLargeError(f">{self._max_response_bytes} bytes")
        try:
            body = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            raise MalformedResponseError("json decode failed") from e
        if not isinstance(body, dict):
            raise MalformedResponseError("body not an object")
        choices = body.get("choices")
        if not isinstance(choices, list) or not choices:
            raise MalformedResponseError("choices missing/empty")
        choice = choices[0]
        if not isinstance(choice, dict):
            raise MalformedResponseError("choice not an object")
        msg = choice.get("message")
        if not isinstance(msg, dict):
            raise MalformedResponseError("message not an object")
        content = msg.get("content")
        if content is not None and not isinstance(content, str):
            raise MalformedResponseError("content wrong type")
        tool_calls = msg.get("tool_calls") or []
        if not isinstance(tool_calls, list):
            raise MalformedResponseError("tool_calls wrong type")
        finish = choice.get("finish_reason")
        return {
            "content": content or "",
            "tool_calls": tool_calls,
            "finish_reason": finish if isinstance(finish, str) else "stop",
        }
