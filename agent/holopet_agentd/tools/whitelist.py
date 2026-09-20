"""holopet_agentd/tools/whitelist.py — R8_R4 受限工具白名单与严格参数校验 (§6.1)。

LLM 暴露的工具只允许来自第 5 节白名单; 参数用严格模式校验, 禁止模型
自由生成系统命令/文件路径/任意 URL。删除全部/清空必须二次确认
(confirm_required 语义由 C++ 服务层二次执行; 本层先拦截模糊危险命令)。
"""
from __future__ import annotations

from holopet_agentd.tools import is_dangerous_name  # 既有危险名策略复用

# 名称 → (必填参数集合, 允许参数集合)
TOOL_WHITELIST: dict[str, tuple[set[str], set[str]]] = {
    "create_timer": ({"duration_ms"}, {"duration_ms", "label"}),
    "list_timers": (set(), set()),
    "cancel_timer": ({"id"}, {"id"}),
    "create_alarm": ({"hour", "minute"}, {"hour", "minute", "repeat", "label"}),
    "list_alarms": (set(), set()),
    "enable_alarm": ({"id", "on"}, {"id", "on"}),
    "delete_alarm": ({"id"}, {"id"}),
    "show_clock": (set(), set()),
    "show_pet": (set(), set()),
    "add_note": ({"text"}, {"text"}),
    "list_notes": (set(), {"limit"}),
    "read_note": ({"id"}, {"id"}),
    "delete_note": ({"id"}, {"id"}),
    # R5_R4 工作包 B2 新增
    "get_ai_mode": (set(), set()),
    "set_ai_mode": ({"mode"}, {"mode"}),
    "dismiss_alert": (set(), set()),
    "remember_preference": ({"key", "value"}, {"key", "value"}),
    "list_preferences": (set(), {"limit"}),
    "forget_preference": ({"id"}, {"id"}),
}


# R8_R4_R1: openai 工具 schema (广告给 LLM 的 tool 列表)。
# TOOL_WHITELIST 只有参数集合没有 schema; TOOL_REGISTRY (tools/__init__.py)
# 只有 V6 内部工具 → 真实链路里白名单工具从未被广告, 模型无法调用
# (Pi B7 实测发现: 直连 API 广告后 Qwen 1.5B 可正常发 tool_call)。
# 以下表为唯一 schema 来源, 参数集合与 TOOL_WHITELIST 一致 (导入期断言)。
_TOOL_SCHEMA_META: dict[str, tuple[str, dict[str, str]]] = {
    "create_timer": (
        "创建一个倒计时定时器; duration_ms 为毫秒数 (如 30 秒=30000)",
        {"duration_ms": "integer", "label": "string"}),
    "list_timers": ("列出当前所有定时器", {}),
    "cancel_timer": ("取消指定 id 的定时器", {"id": "string"}),
    "create_alarm": (
        "新建闹钟; hour(0-23)/minute(0-59) 必填, repeat 可选: "
        "once|daily|workdays, label 可选",
        {"hour": "integer", "minute": "integer",
         "repeat": "string", "label": "string"}),
    "list_alarms": ("列出当前所有闹钟", {}),
    "enable_alarm": ("启用或禁用指定 id 的闹钟, on 为 true/false",
                     {"id": "string", "on": "boolean"}),
    "delete_alarm": ("删除指定 id 的闹钟", {"id": "string"}),
    "show_clock": ("把屏幕切换到常亮时钟模式", {}),
    "show_pet": ("把屏幕切回宠物表情模式", {}),
    "add_note": ("新增一条便签, text 为便签正文", {"text": "string"}),
    "list_notes": ("列出便签列表, limit 可选 (返回条数上限)",
                   {"limit": "integer"}),
    "read_note": ("读取指定 id 的便签正文", {"id": "string"}),
    "delete_note": ("删除指定 id 的便签", {"id": "string"}),
    # R5_R4 工作包 B2 新增
    "get_ai_mode": ("查询当前 AI 模式 (auto|fast|deep|local)", {}),
    "set_ai_mode": ("设置 AI 模式, mode 必须是 auto|fast|deep|local",
                    {"mode": "string"}),
    "dismiss_alert": ("停止当前定时器/闹钟响铃 (不删除配置)", {}),
    "remember_preference": (
        "记住一条用户偏好; key/value 为字符串, value 最多 500 个 Unicode 码点",
        {"key": "string", "value": "string"}),
    "list_preferences": ("列出已记住的偏好, limit 可选 (返回条数上限)",
                         {"limit": "integer"}),
    "forget_preference": ("删除指定 key 的偏好; id='all' 需二次确认",
                          {"id": "string"}),
}


def _build_schemas() -> list:
    out = []
    for name, (desc, props) in _TOOL_SCHEMA_META.items():
        spec = TOOL_WHITELIST.get(name)
        if spec is None:
            raise AssertionError(f"schema meta 无白名单条目: {name}")
        required, allowed = spec
        if set(props) != (required | allowed):
            raise AssertionError(f"{name}: schema 参数集合与白名单不一致")
        out.append({
            "type": "function",
            "function": {
                "name": name,
                "description": desc,
                "parameters": {
                    "type": "object",
                    "properties": {k: {"type": t}
                                   for k, t in props.items()},
                    "required": sorted(required),
                },
            },
        })
    if len(out) != len(TOOL_WHITELIST):
        raise AssertionError("schema 数量与白名单不一致")
    return out


TOOL_SCHEMAS: list = _build_schemas()


def validate_tool_calls(tool_calls: list) -> tuple[list, list]:
    """过滤/校验 LLM tool_calls。

    返回 (accepted, rejected_reasons)。accepted 元素为原始 call dict
    (arguments 已解析为 dict); 未知工具/危险名/缺必填/多余参数 → 拒绝并
    给出稳定短码。重复 tool_call_id 只接受第一次。
    """
    accepted: list = []
    rejected: list = []
    seen_ids: set = set()
    for call in tool_calls or []:
        fn = call.get("function") or {}
        name = str(fn.get("name") or "")
        call_id = str(call.get("id") or "")
        spec = TOOL_WHITELIST.get(name)
        if spec is None:
            rejected.append(("unknown_tool", call_id or name))
            continue
        if is_dangerous_name(name):
            rejected.append(("dangerous_name", call_id or name))
            continue
        if call_id and call_id in seen_ids:
            rejected.append(("duplicate_call_id", call_id))
            continue
        try:
            import json
            args = json.loads(fn.get("arguments") or "{}")
            if not isinstance(args, dict):
                raise ValueError
        except Exception:
            rejected.append(("bad_arguments", call_id or name))
            continue
        required, allowed = spec
        if not required.issubset(set(args)):
            rejected.append(("missing_args", call_id or name))
            continue
        extra = set(args) - allowed
        if extra:
            rejected.append(("unexpected_args", call_id or name))
            continue
        seen_ids.add(call_id)
        accepted.append({**call, "arguments": args})
    return accepted, rejected


def confirm_required(tool_name: str, args: dict) -> bool:
    """危险模糊命令: 删除全部/清空全部必须二次确认 (§6.2, R5_R4 C1)。"""
    if tool_name in ("delete_note", "delete_alarm", "cancel_timer",
                     "forget_preference"):
        if str(args.get("id") or "") == "all":
            return True
    return False


# ============================================================
# R5_R4 工作包 B: 统一工具目录 (单一机器可读来源)
# 每个工具: execution_target (python|cpp) / side_effect
# (none|reversible|destructive) / confirmation_policy / timeout_ms。
# 导入期一致性断言: 目录名 == 白名单 ∪ 内部工具 (不得漂移)。
# ============================================================
def _cat(target, side, confirm, timeout):
    return {"execution_target": target, "side_effect": side,
            "confirmation_policy": confirm, "timeout_ms": timeout}


TOOL_CATALOG: dict[str, dict] = {
    # python 内部工具 (4)
    "get_time": _cat("python", "none", "none", 5000),
    "get_status": _cat("python", "none", "none", 5000),
    "set_volume": _cat("python", "reversible", "none", 5000),
    "set_expression": _cat("python", "reversible", "none", 5000),
    # C++ 功能工具 (13)
    "create_timer": _cat("cpp", "reversible", "none", 10000),
    "list_timers": _cat("cpp", "none", "none", 10000),
    "cancel_timer": _cat("cpp", "destructive", "on_destructive_all", 10000),
    "create_alarm": _cat("cpp", "reversible", "none", 10000),
    "list_alarms": _cat("cpp", "none", "none", 10000),
    "enable_alarm": _cat("cpp", "reversible", "none", 10000),
    "delete_alarm": _cat("cpp", "destructive", "on_destructive_all", 10000),
    "show_clock": _cat("cpp", "none", "none", 10000),
    "show_pet": _cat("cpp", "none", "none", 10000),
    "add_note": _cat("cpp", "reversible", "none", 10000),
    "list_notes": _cat("cpp", "none", "none", 10000),
    "read_note": _cat("cpp", "none", "none", 10000),
    "delete_note": _cat("cpp", "destructive", "on_destructive_all", 10000),
    # R5_R4 新增 C++ 工具 (3)
    "get_ai_mode": _cat("cpp", "none", "none", 10000),
    "set_ai_mode": _cat("cpp", "reversible", "none", 10000),
    "dismiss_alert": _cat("cpp", "reversible", "none", 10000),
    # R5_R4 新增 python 偏好工具 (3, 存储于 agentd SQLite)
    "remember_preference": _cat("python", "reversible", "none", 5000),
    "list_preferences": _cat("python", "none", "none", 5000),
    "forget_preference": _cat("python", "destructive",
                              "on_destructive_all", 5000),
}

# 只读工具 (幂等记录范围外)
READONLY_TOOLS = frozenset(
    n for n, m in TOOL_CATALOG.items() if m["side_effect"] == "none")


def assert_catalog_consistent() -> None:
    """导入期断言: 目录与白名单/内部工具完全一致 (防漂移)。"""
    from holopet_agentd.tools import TOOL_NAMES
    expect = set(TOOL_WHITELIST) | set(TOOL_NAMES)
    if set(TOOL_CATALOG) != expect:
        raise AssertionError(
            "工具目录漂移: %s != %s" % (sorted(set(TOOL_CATALOG)),
                                        sorted(expect)))
    for name, meta in TOOL_CATALOG.items():
        if meta["execution_target"] not in ("python", "cpp"):
            raise AssertionError(f"{name}: 非法 execution_target")
        if meta["side_effect"] not in ("none", "reversible", "destructive"):
            raise AssertionError(f"{name}: 非法 side_effect")
        if meta["confirmation_policy"] not in ("none", "on_destructive_all"):
            raise AssertionError(f"{name}: 非法 confirmation_policy")


assert_catalog_consistent()
