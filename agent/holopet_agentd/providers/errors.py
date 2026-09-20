"""providers/errors.py — Provider 稳定错误类别 (R8_R2 工作包 B)

类别 (code 稳定, 供 TurnRunner → error 事件映射):
  config_error          缺 base_url / provider 配置无效
  missing_api_key       指定环境变量无 Key (不静默回退 Fake)
  auth_error            HTTP 401/403
  rate_limited          HTTP 429
  timeout               请求超时 (socket/read timeout)
  network_error         断连 / DNS / 连接被拒 / 5xx 不可达
  response_too_large    响应体超过 max_response_bytes
  malformed_response    JSON 解析失败 / 缺 choices / 空 choices /
                        message 非对象 / content 类型错

安全要求: message 不包含远端响应正文、URL 查询串、API Key、请求头或
Python 异常细节; 仅类型级描述。
"""


class ProviderError(RuntimeError):
    code = "provider_error"

    def __init__(self, detail: str = ""):
        msg = self.code if not detail else f"{self.code}: {detail}"
        super().__init__(msg)
        self.detail = detail


class ProviderConfigError(ProviderError):
    code = "config_error"


class MissingApiKeyError(ProviderError):
    code = "missing_api_key"


class AuthError(ProviderError):
    code = "auth_error"


class RateLimitedError(ProviderError):
    code = "rate_limited"


class TimeoutError_(ProviderError):
    code = "timeout"


class NetworkError(ProviderError):
    code = "network_error"


class ResponseTooLargeError(ProviderError):
    code = "response_too_large"


class MalformedResponseError(ProviderError):
    code = "malformed_response"


class ModelBlockedError(ProviderError):
    """Provider 拒绝所请求模型 (HTTP 4xx 且错误体点名 model)。

    R8_R4_R3_R4_R5_R3 (工作包 B): 指定模型被拒时稳定上报
    PROVIDER_MODEL_BLOCKED, 绝不静默换模型冒充成功。
    """
    code = "model_blocked"


# TurnRunner 使用的稳定映射 (provider code → 事件 error code)
EVENT_ERROR_CODE = {
    "config_error": "llm_config_error",
    "missing_api_key": "llm_missing_key",
    "auth_error": "llm_auth_error",
    "rate_limited": "llm_rate_limited",
    "timeout": "llm_timeout",
    "network_error": "llm_network_error",
    "response_too_large": "llm_response_too_large",
    "malformed_response": "llm_malformed_response",
    "model_blocked": "PROVIDER_MODEL_BLOCKED",
}


def event_error_code(err: BaseException) -> str:
    """ProviderError → 稳定事件错误码; 未知异常 → llm_failed。"""
    code = getattr(err, "code", "")
    return EVENT_ERROR_CODE.get(code, "llm_failed")
