"""holopet_agentd/router/route_decision.py — R8_R4 简单/复杂模型路由决策。

集中可单测模块 (§7.1): 每轮先确定唯一 route=local_tool|fast|strong|local_llm
并给出审计原因短码。规则 (非字符数唯一判据):
  - 明确本地功能命令 → local_tool / fast (由 tools 编排, 见 route_reason);
  - 日常寒暄/短事实问答/简单改写 → fast;
  - 长文本分析/代码/数学/多约束规划/明确要求深入 → strong;
  - 用户显式 "简单/快速回答" → fast; "深入分析" → strong; "本地处理" → local_llm;
  - 断网 (online=False) → local_llm (仅当 local 可用)。
不并发调用两个付费模型竞速; 升级由调用方显式控制 (fast→strong 最多一次)。
"""
from __future__ import annotations

ROUTE_LOCAL_TOOL = "local_tool"
ROUTE_FAST = "fast"
ROUTE_STRONG = "strong"
ROUTE_LOCAL_LLM = "local_llm"

LOCAL_FEATURE_HINTS = (
    "定时器", "计时", "倒计时", "闹钟", "叫我起床", "提醒我",
    "时钟", "表盘", "便签", "记一下", "记事", "读便签", "看便签",
    "删除便签", "取消定时", "取消闹钟",
)
COMPLEX_HINTS = (
    "代码", "编程", "数学", "证明", "分析", "比较", "总结长文",
    "多步", "规划", "深入", "详细解释", "翻译长文", "写文章",
    "复杂", "推理",
)
EXPLICIT_FAST = ("简单回答", "快速回答", "简短", "一句话")
EXPLICIT_DEEP = ("深入", "仔细想", "认真分析", "多想想")
EXPLICIT_LOCAL = ("本地处理", "本地模型", "离线回答")


def route_decision(text: str, online: bool,
                   explicit: str | None = None) -> tuple[str, str]:
    """返回 (route, reason_code)。explicit 为已识别的用户显式覆盖或 None。"""
    t = (text or "")
    if not online:
        return ROUTE_LOCAL_LLM, "offline_local"
    if explicit:
        if explicit == "fast":
            return ROUTE_FAST, "explicit_fast"
        if explicit == "deep":
            return ROUTE_STRONG, "explicit_deep"
        if explicit == "local":
            return ROUTE_LOCAL_LLM, "explicit_local"
    low = t.lower()
    for h in EXPLICIT_LOCAL:
        if h in t:
            return ROUTE_LOCAL_LLM, "explicit_local"
    for h in EXPLICIT_DEEP:
        if h in t:
            return ROUTE_STRONG, "explicit_deep"
    for h in EXPLICIT_FAST:
        if h in t or h in low:
            return ROUTE_FAST, "explicit_fast"
    for h in LOCAL_FEATURE_HINTS:
        if h in t:
            return ROUTE_FAST, "local_feature_fast"
    for h in COMPLEX_HINTS:
        if h in t:
            return ROUTE_STRONG, "complex_hint"
    if len(t) > 600:
        return ROUTE_STRONG, "long_text"
    if len(t) <= 120:
        return ROUTE_FAST, "short_greeting"
    return ROUTE_FAST, "default_fast"


def detect_explicit_override(text: str) -> str | None:
    """识别用户显式路由覆盖; 无覆盖返回 None。"""
    for h in EXPLICIT_LOCAL:
        if h in (text or ""):
            return "local"
    for h in EXPLICIT_DEEP:
        if h in (text or ""):
            return "deep"
    for h in EXPLICIT_FAST:
        if h in (text or ""):
            return "fast"
    return None


# R8_R4_R2: 快速模型"能力不足"响应标记 (升级触发词, 可配置覆盖)。
# 命中任意一条 → fast→strong 最多一次; 不因长度/关键词误判为升级。
DEFAULT_INSUFFICIENT_MARKERS = (
    "能力不足", "无法回答", "回答不了", "超出我的能力",
    "需要更强的模型", "这个问题太复杂", "我无法完成",
)


def detect_insufficient(content: str, markers=None) -> bool:
    """快速模型响应是否表达"能力不足/需深入"(§7.2 升级触发判定)。"""
    text = content or ""
    if not text.strip():
        return False
    for m in markers or DEFAULT_INSUFFICIENT_MARKERS:
        if m in text:
            return True
    return False
