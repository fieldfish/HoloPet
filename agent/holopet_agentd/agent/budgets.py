# -*- coding: utf-8 -*-
"""agent/budgets.py — R5_R4 工作包 A: 工具轮次与调用预算。

预算规则：最多 4 个 Agent 工具轮次、每轮最多 4 个工具、整轮最多 8 次工具
执行; 达到上限返回稳定错误 agent_budget_exceeded, 不得无限循环。
"""
from __future__ import annotations


class BudgetExceeded(RuntimeError):
    """预算耗尽 (稳定短码, 不携带内部细节到用户侧)。"""

    def __init__(self, limit: str, used: int, cap: int):
        super().__init__("agent_budget_exceeded")
        self.code = "agent_budget_exceeded"
        self.limit = limit
        self.used = used
        self.cap = cap


class AgentBudgets:
    MAX_TOOL_ROUNDS = 4
    MAX_TOOLS_PER_ROUND = 4
    MAX_TOTAL_TOOL_EXEC = 8

    def __init__(self,
                 max_rounds: int = MAX_TOOL_ROUNDS,
                 max_per_round: int = MAX_TOOLS_PER_ROUND,
                 max_total: int = MAX_TOTAL_TOOL_EXEC):
        if max_rounds < 0 or max_per_round < 0 or max_total < 0:
            raise ValueError("budgets 不得为负")
        self.max_rounds = max_rounds
        self.max_per_round = max_per_round
        self.max_total = max_total
        self.rounds_used = 0
        self.round_tools = 0
        self.total_exec = 0

    def begin_round(self) -> None:
        """进入新工具轮次; 超限抛 BudgetExceeded。"""
        if self.rounds_used >= self.max_rounds:
            raise BudgetExceeded("rounds", self.rounds_used, self.max_rounds)
        self.rounds_used += 1
        self.round_tools = 0

    def charge_tool(self, name: str) -> None:
        """登记一次工具执行 (每轮 + 总量双限)。"""
        if self.round_tools >= self.max_per_round:
            raise BudgetExceeded("per_round", self.round_tools,
                                 self.max_per_round)
        if self.total_exec >= self.max_total:
            raise BudgetExceeded("total", self.total_exec, self.max_total)
        self.round_tools += 1
        self.total_exec += 1

    def reset(self) -> None:
        self.rounds_used = 0
        self.round_tools = 0
        self.total_exec = 0

    def snapshot(self) -> dict:
        return {"rounds_used": self.rounds_used,
                "round_tools": self.round_tools,
                "total_exec": self.total_exec}
