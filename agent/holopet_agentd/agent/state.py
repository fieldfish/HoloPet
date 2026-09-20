# -*- coding: utf-8 -*-
"""agent/state.py — R5_R4 工作包 A: Agent 状态机。

状态流：
  IDLE → ACCEPT_INPUT → ROUTE → THINK_OR_FAST_PATH → REQUEST_TOOL
  → WAIT_TOOL_RESULT → OBSERVE_RESULT → THINK_NEXT_OR_RESPOND
  → DISPLAY_AND_TTS → COMPLETE
旁路: WAIT_CONFIRMATION / CANCELLED / TOOL_FAILED / MODEL_FAILED / TIMEOUT

本模块只做合法性判定 (纯函数, 可单测): 非法转移抛 IllegalTransition,
调用方负责按合法转移驱动流程。
"""
from __future__ import annotations

from enum import Enum


class AgentState(Enum):
    IDLE = "idle"
    ACCEPT_INPUT = "accept_input"
    ROUTE = "route"
    THINK = "think"                      # 模型请求/规划 (含 fast path)
    REQUEST_TOOL = "request_tool"
    WAIT_TOOL_RESULT = "wait_tool_result"
    OBSERVE_RESULT = "observe_result"
    THINK_NEXT = "think_next"
    RESPOND = "respond"                  # 最终答复 (DISPLAY_AND_TTS)
    COMPLETE = "complete"
    # 旁路
    WAIT_CONFIRMATION = "wait_confirmation"
    CANCELLED = "cancelled"
    TOOL_FAILED = "tool_failed"
    MODEL_FAILED = "model_failed"
    TIMEOUT = "timeout"


TERMINAL = frozenset({
    AgentState.COMPLETE, AgentState.CANCELLED, AgentState.TOOL_FAILED,
    AgentState.MODEL_FAILED, AgentState.TIMEOUT,
})

# 合法转移表 (from → to 集合)
TRANSITIONS = {
    AgentState.IDLE: {AgentState.ACCEPT_INPUT, AgentState.CANCELLED},
    AgentState.ACCEPT_INPUT: {AgentState.ROUTE, AgentState.CANCELLED},
    AgentState.ROUTE: {AgentState.THINK, AgentState.MODEL_FAILED,
                       AgentState.CANCELLED},
    AgentState.THINK: {AgentState.REQUEST_TOOL, AgentState.RESPOND,
                       AgentState.MODEL_FAILED, AgentState.TIMEOUT,
                       AgentState.CANCELLED},
    AgentState.REQUEST_TOOL: {AgentState.WAIT_TOOL_RESULT,
                              AgentState.WAIT_CONFIRMATION,
                              AgentState.TOOL_FAILED, AgentState.CANCELLED},
    AgentState.WAIT_TOOL_RESULT: {AgentState.OBSERVE_RESULT,
                                  AgentState.REQUEST_TOOL,   # 同轮下一工具
                                  AgentState.TOOL_FAILED,
                                  AgentState.TIMEOUT, AgentState.CANCELLED},
    AgentState.OBSERVE_RESULT: {AgentState.THINK_NEXT, AgentState.RESPOND,
                                AgentState.CANCELLED},
    AgentState.THINK_NEXT: {AgentState.REQUEST_TOOL, AgentState.RESPOND,
                            AgentState.MODEL_FAILED, AgentState.TIMEOUT,
                            AgentState.CANCELLED},
    AgentState.RESPOND: {AgentState.COMPLETE, AgentState.CANCELLED},
    AgentState.WAIT_CONFIRMATION: {AgentState.REQUEST_TOOL,
                                   AgentState.CANCELLED,
                                   AgentState.TOOL_FAILED},  # 过期 → 稳定码
    # 终态: 只允许收敛到 COMPLETE 前的记录性转移; 到达终态后不再转移
    AgentState.CANCELLED: set(),
    AgentState.TOOL_FAILED: {AgentState.COMPLETE},
    AgentState.MODEL_FAILED: {AgentState.COMPLETE},
    AgentState.TIMEOUT: {AgentState.COMPLETE},
    AgentState.COMPLETE: set(),
}


class IllegalTransition(RuntimeError):
    """非法状态转移 (fail closed)。"""

    def __init__(self, frm, to):
        super().__init__(f"IllegalTransition: %s -> %s" % (frm, to))
        self.frm = frm
        self.to = to


class AgentStateMachine:
    """单活动轮状态机: 一个 request_id 一轮, 单一 terminal。"""

    def __init__(self):
        self._state = AgentState.IDLE
        self._terminal_seen = False

    @property
    def state(self) -> AgentState:
        return self._state

    @property
    def terminal_seen(self) -> bool:
        return self._terminal_seen

    def can(self, to: AgentState) -> bool:
        return to in TRANSITIONS.get(self._state, set())

    def transition(self, to: AgentState) -> AgentState:
        if not self.can(to):
            raise IllegalTransition(self._state, to)
        self._state = to
        if to in TERMINAL:
            self._terminal_seen = True
        return to

    def reset(self) -> None:
        self._state = AgentState.IDLE
        self._terminal_seen = False
