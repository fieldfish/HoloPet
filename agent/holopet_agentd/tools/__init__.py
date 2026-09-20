"""tools/__init__.py — 工具白名单注册表  V6

所有工具静态注册并进入显式白名单; 不提供 shell/exec 类工具。
与 C++ src/agent/tool_policy.hpp 的名单保持一致。
"""

import datetime

# 危险名称模式 (与 C++ ToolPolicy::isDangerous 一致)
_DANGEROUS_PATTERNS = ("shell", "exec", "subprocess", "system",
                       "popen", "eval", "os.", "import", "__")


def is_dangerous_name(name: str) -> bool:
    return any(p in name for p in _DANGEROUS_PATTERNS)


def _tool_get_time(arguments: dict):
    del arguments
    return {"time": datetime.datetime.now().strftime("%H:%M")}


def _tool_get_status(arguments: dict):
    del arguments
    return {"status": "idle"}


def _tool_set_volume(arguments: dict):
    # 本地占位: 真实音量由 C++ 主程序控制 (worker 只回执)
    vol = arguments.get("volume", 0.5)
    return {"volume": max(0.0, min(1.0, float(vol)))}


def _tool_set_expression(arguments: dict):
    emo = arguments.get("emotion", "neutral")
    return {"emotion": emo}


# 白名单注册表: name → {"schema": openai-tool-schema, "handler": fn}
def _schema(name: str, desc: str, props: dict, required: list) -> dict:
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": desc,
            "parameters": {"type": "object",
                           "properties": props,
                           "required": required},
        },
    }


TOOL_NAMES = {
    "get_time":       {"schema": _schema("get_time", "获取当前时间", {}, []),
                       "handler": _tool_get_time},
    "get_status":     {"schema": _schema("get_status", "查询设备状态", {}, []),
                       "handler": _tool_get_status},
    "set_volume":     {"schema": _schema("set_volume", "设置音量 0..1",
                                         {"volume": {"type": "number"}}, ["volume"]),
                       "handler": _tool_set_volume},
    "set_expression": {"schema": _schema("set_expression", "设置情绪表情",
                                         {"emotion": {"type": "string"}}, ["emotion"]),
                       "handler": _tool_set_expression},
}

# 向模型暴露的 schema 列表 (openai tools 格式)
TOOL_REGISTRY = [v["schema"] for v in TOOL_NAMES.values()]


def is_allowed(name: str) -> bool:
    return name in TOOL_NAMES and not is_dangerous_name(name)


def dispatch(name: str, arguments: dict):
    """白名单分发; 危险名/未注册名拒绝 (raise ValueError)"""
    if is_dangerous_name(name) or name not in TOOL_NAMES:
        raise ValueError(f"tool rejected: {name}")
    return TOOL_NAMES[name]["handler"](arguments or {})
