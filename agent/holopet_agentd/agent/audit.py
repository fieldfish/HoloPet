# -*- coding: utf-8 -*-
"""agent/audit.py — R5_R4 工作包 A1: Agent 审计事件。

审计事件至少包括 agent_turn_started / agent_route_selected /
agent_plan_started / agent_tool_requested / agent_tool_accepted /
agent_tool_rejected / agent_tool_result / agent_confirmation_required /
agent_confirmation_accepted / agent_confirmation_cancelled /
agent_confirmation_expired / agent_observation_applied /
agent_final_started / agent_turn_completed / agent_turn_cancelled /
agent_turn_failed。

每条至少带 request_id、单调时间、阶段、模型别名、工具名/短码; 正文只记录
长度和 SHA256 摘要, 不记录内容本身。
"""
from __future__ import annotations

import hashlib
import json
import time
from typing import Optional

VALID_EVENTS = frozenset({
    "agent_turn_started", "agent_route_selected", "agent_plan_started",
    "agent_tool_requested", "agent_tool_accepted", "agent_tool_rejected",
    "agent_tool_result", "agent_confirmation_required",
    "agent_confirmation_accepted", "agent_confirmation_cancelled",
    "agent_confirmation_expired", "agent_observation_applied",
    "agent_final_started", "agent_turn_completed", "agent_turn_cancelled",
    "agent_turn_failed",
})


def digest_of(text: str) -> str:
    """正文摘要 (SHA256), 不存内容本身。"""
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


class AgentAudit:
    """Agent 审计器: 内存队列 + 可选 JSONL 文件下沉 (每行一条, 无正文)。"""

    def __init__(self, file_path: Optional[str] = None):
        self._events: list = []
        self._file = file_path
        self._clock = time.monotonic

    @property
    def events(self):
        return list(self._events)

    def record(self, event: str, *, request_id: str, stage: str,
               model: str = "", tool: str = "", code: str = "",
               content_text: str = "", **fields) -> None:
        """追加一条审计事件; 未知事件名 fail closed。"""
        if event not in VALID_EVENTS:
            raise ValueError("unknown audit event: %s" % event)
        doc = {
            "event": event,
            "request_id": request_id,
            "t_ms": int(self._clock() * 1000),
            "stage": stage,
        }
        if model:
            doc["model"] = model
        if tool:
            doc["tool"] = tool
        if code:
            doc["code"] = code
        if content_text:
            doc["content_chars"] = len(content_text)
            doc["content_sha256"] = digest_of(content_text)
        for k, v in fields.items():
            if v is not None:
                doc[k] = v
        self._events.append(doc)
        if self._file:
            try:
                with open(self._file, "a", encoding="utf-8") as f:
                    f.write(json.dumps(doc, ensure_ascii=False) + "\n")
            except OSError:
                pass  # 审计下沉失败不得阻断主流程

    def summary(self) -> dict:
        """事件计数摘要 (不含正文)。"""
        counts = {}
        for e in self._events:
            counts[e["event"]] = counts.get(e["event"], 0) + 1
        return counts
