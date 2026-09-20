"""providers/fake.py — FakeAgentProvider (不联网, 离线可用)  V6-R2

接口: chat(messages) -> {"content": str, "tool_calls": [...], "finish_reason": str}
工具回灌由 worker 的 turn runner 统一完成 (provider 不自行追加 tool 消息)。
"""

import json

from ..tools import TOOL_REGISTRY


class FakeAgentProvider:
    """无网络 Fake Provider; 与 OpenAICompatibleProvider 同接口。"""

    def __init__(self, config: dict):
        self._config = config
        self.max_tool_rounds = int(config.get("max_tool_rounds", 4))
        # R4 (P3): 故障注入 — {"llm_inject": {"timeout": true}}
        self._inject_timeout = bool((config.get("llm_inject") or {}).get("timeout"))

    def reset(self) -> None:
        pass

    def tool_schemas(self) -> list:
        return TOOL_REGISTRY

    def chat(self, messages: list) -> dict:
        """单次模型调用 (无网络)。messages: [{"role","content",...}]"""
        if self._inject_timeout:
            raise TimeoutError("llm_inject.timeout")
        user_text = ""
        for m in reversed(messages):
            if m.get("role") == "user":
                user_text = str(m.get("content", ""))
                break
        # 若上一条 assistant 带 tool 消息, 说明工具已回灌 → 用其结果续答
        tool_content = None
        for m in reversed(messages):
            if m.get("role") == "tool":
                tool_content = str(m.get("content", ""))
                break

        t = user_text.strip().lower()

        if any(k in t for k in ("几点", "时间", "time")):
            if not tool_content:
                return {"content": None,
                        "tool_calls": [{"id": "call-1", "function":
                                        {"name": "get_time", "arguments": "{}"}}],
                        "finish_reason": "tool_calls"}
            return {"content": f"现在是{tool_content}。", "tool_calls": [],
                    "finish_reason": "stop"}

        if any(k in t for k in ("开心", "高兴", "happy")):
            if not tool_content:
                return {"content": None,
                        "tool_calls": [{"id": "call-1", "function":
                                        {"name": "set_expression",
                                         "arguments": "{\"emotion\":\"happy\"}"}}],
                        "finish_reason": "tool_calls"}
            # R4-R1 (D10): set_expression 工具结果真实生效 —
            # 回灌后携带实际 emotion, 不再固定 neutral
            emotion = "neutral"
            try:
                tr = json.loads(tool_content)
                if isinstance(tr, dict) and str(tr.get("emotion", "")).strip():
                    emotion = str(tr["emotion"]).strip()
            except (json.JSONDecodeError, ValueError):
                pass
            return {"content": "好的，我开心起来啦！", "tool_calls": [],
                    "finish_reason": "stop", "emotion": emotion}

        if any(k in t for k in ("难过", "伤心", "sad")):
            return {"content": "听起来有点低落。需要我陪你聊聊天吗？", "tool_calls": [],
                    "finish_reason": "stop"}
        if "你好" in t or "hello" in t or "hi" in t:
            return {"content": "你好呀，我是 HoloPet。按下旋钮就可以和我说话。", "tool_calls": [],
                    "finish_reason": "stop"}
        return {"content": ("我听到了。我是 HoloPet 的离线演示模式，"
                            "接入 OpenAI 兼容接口后就能真正聊天啦。"),
                "tool_calls": [], "finish_reason": "stop"}
