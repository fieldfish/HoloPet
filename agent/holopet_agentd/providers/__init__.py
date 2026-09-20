"""providers/__init__.py — Provider 工厂"""

from .fake import FakeAgentProvider
from .openai_compat import OpenAICompatibleProvider


def make_provider(config: dict):
    """按 config.provider 构造 (fake 默认, 离线可用)"""
    kind = (config.get("provider") or "fake").lower()
    if kind in ("fake", "openai_compatible"):
        if kind == "openai_compatible":
            return OpenAICompatibleProvider(config)
        return FakeAgentProvider(config)
    # R4 (P3): 未知 provider → 显式 ValueError (上层转稳定错误码)
    raise ValueError(f"unknown provider: {kind}")


__all__ = ["FakeAgentProvider", "OpenAICompatibleProvider", "make_provider"]
