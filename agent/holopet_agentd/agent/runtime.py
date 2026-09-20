# -*- coding: utf-8 -*-
"""agent/runtime.py — R5_R4 工作包 A: 独立 Agent Runtime。

依赖注入: model_client / tool_executor / 时钟 / 取消事件 / 审计 / 预算 /
确认门 / 幂等存储。main.py 只负责配置、连接、语音链编排与事件转发。

职责:
  - 驱动 §3 状态机 (规划 → 工具 → 观察 → 下一步/答复);
  - 工具轮次与总量预算 (agent_budget_exceeded);
  - 每轮恰一个 terminal 结果; 取消后不再调工具/TTS/写历史;
  - 工具调用唯一 tool_call_id; 结果按同 id 回填; 重复/迟到结果只消费一次;
  - Provider 格式错误最多一次结构修复请求;
  - 破坏性 "全部" 工具进入二次确认 (确认后原参数恰好一次)。
"""
from __future__ import annotations

import hashlib
import json
import time
import uuid
from typing import Callable, Optional

from .audit import AgentAudit
from .budgets import AgentBudgets, BudgetExceeded
from .confirmation import ConfirmationGate
from .session_store import AgentStore
from .state import (AgentState, AgentStateMachine, IllegalTransition,
                    TERMINAL)

AGENT_BUDGET_CODE = "agent_budget_exceeded"
CONFIRM_EXPIRED_CODE = "confirmation_expired"
CANCEL_CODE = "cancelled"
MODEL_FORMAT_CODE = "model_format_error"

# R5_R4 (E3): 确定性快速路径 — 完全确定、无歧义的命令 (精确相等匹配,
# 不含时间/数字/删除/复合条件; 其余自然语言一律交给 Agent)。
DETERMINISTIC_FAST = {
    "show_clock": ("显示时钟", "切到时钟", "切换到时钟"),
    "show_pet": ("返回宠物", "显示宠物"),
    "dismiss_alert": ("停止响铃", "别响了", "停止响", "关闭响铃"),
    "get_ai_mode": ("查询当前 AI 模式", "当前 AI 模式"),
}
DETERMINISTIC_FAST_ANSWER = {
    "show_clock": "已显示时钟",
    "show_pet": "已返回宠物",
    "dismiss_alert": "已停止响铃",
    "get_ai_mode": "当前 AI 模式：{mode}",
}


class ModelClient:            # pragma: no cover — 接口文档 (鸭子类型)
    def chat(self, messages: list) -> dict:
        """返回 {"content","tool_calls","finish_reason"}; 失败抛 ProviderError。"""
        raise NotImplementedError


class ToolExecutor:           # pragma: no cover — 接口文档 (鸭子类型)
    def execute(self, tool_name: str, arguments: dict,
                call_id: str) -> dict:
        """返回 {"ok": bool, "code": str, "result": dict}; 感知取消。"""
        raise NotImplementedError

    def validate(self, tool_calls: list) -> tuple:
        """白名单/参数校验: 返回 (accepted, rejected)。"""
        raise NotImplementedError

    def needs_confirmation(self, tool_name: str, arguments: dict) -> bool:
        raise NotImplementedError

    def has_side_effect(self, tool_name: str) -> bool:
        """副作用工具 (幂等记录范围); 读取类返回 False。"""
        raise NotImplementedError


def args_digest(arguments: dict) -> str:
    blob = json.dumps(arguments, sort_keys=True,
                      ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


class TurnOutcome:
    """一轮 Agent 的收口结果 (恰一个 terminal)。"""

    def __init__(self, *, state: AgentState, answer_text: str = "",
                 code: str = "", final_only: bool = True,
                 resp_extra: Optional[dict] = None):
        self.state = state
        self.answer_text = answer_text
        self.code = code
        self.final_only = final_only
        self.resp_extra = resp_extra or {}


class AgentRuntime:
    def __init__(self, *,
                 model_client: ModelClient,
                 tool_executor: ToolExecutor,
                 audit: Optional[AgentAudit] = None,
                 budgets: Optional[AgentBudgets] = None,
                 confirmation: Optional[ConfirmationGate] = None,
                 store: Optional[AgentStore] = None,
                 clock=None,
                 max_fixup_requests: int = 1,
                 escalate_cb: Optional[Callable[[], object]] = None,
                 insufficient_markers: tuple = ()):
        self._model = model_client
        self._tools = tool_executor
        self._audit = audit or AgentAudit()
        self._budgets = budgets or AgentBudgets()
        self._clock = clock or time.monotonic
        # 确认门必须共享同一注入时钟 (TTL 判定一致性)
        self._confirmation = confirmation or ConfirmationGate(
            clock=self._clock)
        self._store = store
        self._max_fixup = max_fixup_requests
        self._escalate_cb = escalate_cb
        self._insufficient = tuple(insufficient_markers)

    # ---- 事件发射 (由 main.py 注入或测试直接收集) ----
    def _emit(self, emit, event: str, **fields) -> None:
        if emit is not None:
            emit(event, **fields)

    def run_turn(self, *, request_id: str, user_text: str, model_alias: str,
                 system_text: str, cancel_event, emit=None,
                 history_turns: Optional[list] = None) -> TurnOutcome:
        """完整 Agent 轮 (工具循环)。cancel_event 可随时中止。"""
        sm = AgentStateMachine()
        self._budgets.reset()
        self._audit.record("agent_turn_started", request_id=request_id,
                           stage="accept_input", model=model_alias,
                           content_text=user_text)
        try:
            sm.transition(AgentState.ACCEPT_INPUT)
            sm.transition(AgentState.ROUTE)
            self._audit.record("agent_route_selected", request_id=request_id,
                               stage="route", model=model_alias)
            sm.transition(AgentState.THINK)
        except IllegalTransition as e:
            return TurnOutcome(state=AgentState.MODEL_FAILED,
                               code="state_error")
        except Exception:
            return TurnOutcome(state=AgentState.MODEL_FAILED,
                               code="state_error")

        messages = [{"role": "system", "content": system_text}]
        if history_turns:
            for h in history_turns[-16:]:
                messages.append(h)
        messages.append({"role": "user", "content": user_text})

        # R5_R4 (E3): 确定性快速路径 — 同一执行器/审计/幂等/回执, 免模型
        fast_tool = self._deterministic_fast(user_text)
        if fast_tool is not None:
            return self._run_deterministic_fast(
                request_id, fast_tool, sm, cancel_event)

        escalated = False
        fixups = 0
        executed_any = False
        tool_failed_this_turn = False
        while True:
            if cancel_event.is_set():
                self._audit.record("agent_turn_cancelled",
                                   request_id=request_id, stage="cancelled")
                return TurnOutcome(state=AgentState.CANCELLED,
                                   code=CANCEL_CODE)
            # 模型请求
            try:
                resp = self._model.chat(messages)
            except TimeoutError:
                sm.transition(AgentState.TIMEOUT)
                self._audit.record("agent_turn_failed", request_id=request_id,
                                   stage="think", code="timeout")
                return TurnOutcome(state=AgentState.TIMEOUT, code="timeout")
            except Exception as e:
                code = getattr(e, "code", None) or "model_failed"
                sm.transition(AgentState.MODEL_FAILED)
                self._audit.record("agent_turn_failed", request_id=request_id,
                                   stage="think", code=code)
                return TurnOutcome(state=AgentState.MODEL_FAILED, code=code)

            content = resp.get("content") or ""
            tool_calls = resp.get("tool_calls") or []
            if not isinstance(tool_calls, list) or not isinstance(
                    content, str):
                # 结构格式错误: 最多一次修复请求, 不得无限重试
                if fixups < self._max_fixup:
                    fixups += 1
                    messages.append({
                        "role": "assistant",
                        "content": "（系统提示）请按工具调用格式返回。",
                    })
                    continue
                sm.transition(AgentState.MODEL_FAILED)
                self._audit.record("agent_turn_failed", request_id=request_id,
                                   stage="think", code=MODEL_FORMAT_CODE)
                return TurnOutcome(state=AgentState.MODEL_FAILED,
                                   code=MODEL_FORMAT_CODE)

            if tool_calls:
                # 工具失败后本轮阻断后续工具 (fail closed): 模型只允许给出
                # 基于真实错误的最终答复, 不得继续链式依赖动作
                if tool_failed_this_turn:
                    self._audit.record("agent_turn_failed",
                                       request_id=request_id,
                                       stage="request_tool",
                                       code="tool_chain_blocked")
                    return TurnOutcome(state=AgentState.TOOL_FAILED,
                                       code="tool_chain_blocked")
                # 工具轮次
                try:
                    self._budgets.begin_round()
                except BudgetExceeded:
                    return self._budget_exhausted(request_id, sm)
                accepted, rejected = self._tools.validate(tool_calls)
                for name, why in rejected:
                    self._audit.record("agent_tool_rejected",
                                       request_id=request_id,
                                       stage="request_tool", tool=name,
                                       code=why)
                last_ok = True
                last_error = ""
                messages.append({"role": "assistant",
                                 "content": content,
                                 "tool_calls": tool_calls})
                for call in accepted:
                    fn = call.get("function") or {}
                    name = fn.get("name", "")
                    call_id = call.get("id") or ("gen-" + uuid.uuid4().hex)
                    args = fn.get("arguments") or {}
                    try:
                        self._budgets.charge_tool(name)
                    except BudgetExceeded:
                        return self._budget_exhausted(request_id, sm)
                    sm.transition(AgentState.REQUEST_TOOL)
                    # 二次确认
                    if self._tools.needs_confirmation(name, args):
                        self._confirmation.request(
                            request_id=request_id, tool_name=name,
                            normalized_args=dict(args),
                            raw_request={"call_id": call_id},
                            tool_call_id=call_id)
                        sm.transition(AgentState.WAIT_CONFIRMATION)
                        self._audit.record("agent_confirmation_required",
                                           request_id=request_id,
                                           stage="wait_confirmation",
                                           tool=name)
                        return TurnOutcome(
                            state=AgentState.WAIT_CONFIRMATION,
                            answer_text="请确认是否执行：%s" % name,
                            code="pending_confirmation")
                    self._audit.record("agent_tool_requested",
                                       request_id=request_id,
                                       stage="request_tool", tool=name)
                    if cancel_event.is_set():
                        self._audit.record("agent_turn_cancelled",
                                           request_id=request_id,
                                           stage="cancelled")
                        return TurnOutcome(state=AgentState.CANCELLED,
                                           code=CANCEL_CODE)
                    sm.transition(AgentState.WAIT_TOOL_RESULT)
                    # D3 重启幂等: 副作用工具执行前查重放记录
                    if (self._store is not None
                            and self._tools.has_side_effect(name)
                            and self._store.lookup_idempotent(
                                request_id, call_id) is not None):
                        self._audit.record("agent_tool_result",
                                           request_id=request_id,
                                           stage="observe_result",
                                           tool=name,
                                           code="duplicate_ignored")
                        messages.append({
                            "role": "tool", "tool_call_id": call_id,
                            "content": json.dumps(
                                {"error": "duplicate_ignored"},
                                ensure_ascii=False)})
                        continue
                    res = self._tools.execute(name, args, call_id)
                    if (self._store is not None
                            and self._tools.has_side_effect(name)):
                        try:
                            self._store.record_idempotent(
                                request_id=request_id,
                                tool_call_id=call_id,
                                args_hash=args_digest(args),
                                result_code=res.get("code") if res.get("ok")
                                else res.get("code") or "tool_failed",
                                result_digest=args_digest(
                                    res.get("result") or {}))
                        except Exception:
                            pass      # 幂等下沉失败不阻断主流程
                    if not res.get("ok"):
                        last_ok = False
                        last_error = res.get("code") or "tool_failed"
                        tool_failed_this_turn = True
                        # 失败不置终态: 回填真实错误后让模型给最终答复
                        sm.transition(AgentState.OBSERVE_RESULT)
                        sm.transition(AgentState.THINK_NEXT)
                        self._audit.record("agent_tool_result",
                                           request_id=request_id,
                                           stage="observe_result",
                                           tool=name, code=last_error)
                        messages.append({
                            "role": "tool", "tool_call_id": call_id,
                            "content": json.dumps(
                                {"error": last_error},
                                ensure_ascii=False)})
                        break
                    executed_any = True
                    self._audit.record("agent_tool_result",
                                       request_id=request_id,
                                       stage="observe_result", tool=name,
                                       code="ok")
                    self._audit.record("agent_observation_applied",
                                       request_id=request_id,
                                       stage="observe_result", tool=name)
                    messages.append({
                        "role": "tool", "tool_call_id": call_id,
                        "content": json.dumps(
                            res.get("result") or {}, ensure_ascii=False)})
                if last_ok and executed_any:
                    sm.transition(AgentState.OBSERVE_RESULT)
                    sm.transition(AgentState.THINK_NEXT)
                continue
            # 无工具调用: 最终答复 (或 fast→strong 升级, 最多一次)
            if (self._escalate_cb is not None and not escalated
                    and not executed_any and not tool_failed_this_turn
                    and self._looks_insufficient(content)):
                escalated = True
                self._model = self._escalate_cb()
                self._audit.record("agent_plan_started",
                                   request_id=request_id, stage="think",
                                   code="fast_to_strong")
                continue
            if content:
                sm.transition(AgentState.RESPOND)
                self._audit.record("agent_final_started",
                                   request_id=request_id, stage="respond",
                                   model=model_alias, content_text=content)
                if self._store is not None:
                    try:
                        self._store.add_summary(request_id, content)
                    except Exception:
                        pass
                self._audit.record("agent_turn_completed",
                                   request_id=request_id, stage="respond")
                return TurnOutcome(state=AgentState.COMPLETE,
                                   answer_text=content, resp_extra=resp)
            # 空答复
            sm.transition(AgentState.RESPOND)
            self._audit.record("agent_turn_failed", request_id=request_id,
                               stage="respond", code="empty_answer")
            return TurnOutcome(state=AgentState.COMPLETE,
                               answer_text="", code="empty_answer")

    def _budget_exhausted(self, request_id, sm) -> TurnOutcome:
        self._audit.record("agent_turn_failed", request_id=request_id,
                           stage="request_tool", code=AGENT_BUDGET_CODE)
        return TurnOutcome(state=AgentState.TOOL_FAILED,
                           code=AGENT_BUDGET_CODE)

    @staticmethod
    def _deterministic_fast(text: str):
        t = (text or "").strip()
        for tool, phrases in DETERMINISTIC_FAST.items():
            if t in phrases:
                return tool
        return None

    def _run_deterministic_fast(self, request_id, tool_name, sm,
                                cancel_event) -> TurnOutcome:
        """E3: 确定性快速路径 — 走同一工具执行器/预算/审计/幂等。"""
        self._audit.record("agent_route_selected", request_id=request_id,
                           stage="route", code="deterministic_fast")
        self._audit.record("agent_plan_started", request_id=request_id,
                           stage="request_tool", tool=tool_name)
        try:
            self._budgets.begin_round()
            self._budgets.charge_tool(tool_name)
        except BudgetExceeded:
            return self._budget_exhausted(request_id, sm)
        sm.transition(AgentState.REQUEST_TOOL)
        sm.transition(AgentState.WAIT_TOOL_RESULT)
        call_id = "fast-" + uuid.uuid4().hex[:8]
        self._audit.record("agent_tool_requested", request_id=request_id,
                           stage="request_tool", tool=tool_name)
        res = self._tools.execute(tool_name, {}, call_id)
        if not res.get("ok"):
            self._audit.record("agent_tool_result", request_id=request_id,
                               stage="observe_result", tool=tool_name,
                               code=res.get("code") or "tool_failed")
            sm.transition(AgentState.OBSERVE_RESULT)
            sm.transition(AgentState.THINK_NEXT)
            return TurnOutcome(state=AgentState.TOOL_FAILED,
                               code=res.get("code") or "tool_failed")
        self._audit.record("agent_tool_result", request_id=request_id,
                           stage="observe_result", tool=tool_name, code="ok")
        self._audit.record("agent_observation_applied",
                           request_id=request_id, stage="observe_result",
                           tool=tool_name)
        answer = DETERMINISTIC_FAST_ANSWER.get(tool_name, "已执行")
        result = res.get("result") or {}
        if tool_name == "get_ai_mode":
            mode = str(result.get("mode") or "unknown")
            answer = DETERMINISTIC_FAST_ANSWER["get_ai_mode"].format(
                mode=mode)
        sm.transition(AgentState.OBSERVE_RESULT)
        sm.transition(AgentState.RESPOND)
        self._audit.record("agent_final_started", request_id=request_id,
                           stage="respond", content_text=answer)
        self._audit.record("agent_turn_completed", request_id=request_id,
                           stage="respond")
        return TurnOutcome(state=AgentState.COMPLETE, answer_text=answer,
                           resp_extra={})

    def _looks_insufficient(self, content: str) -> bool:
        if not self._insufficient or not (content or "").strip():
            return False
        for m in self._insufficient:
            if m in content:
                return True
        return False

    # ---- 确认回合 (用户下一条输入解释) ----
    def handle_confirmation(self, *, request_id: str, user_text: str,
                            model_alias: str, cancel_event,
                            emit=None) -> TurnOutcome:
        """WAIT_CONFIRMATION 的后续输入处理。

        accept → 用原参数经同一执行器执行恰好一次 (幂等记录);
        cancel → 无副作用; ask_again → 重新询问; expired → confirmation_expired。
        """
        sm = AgentStateMachine()
        for s in (AgentState.ACCEPT_INPUT, AgentState.ROUTE,
                  AgentState.THINK, AgentState.REQUEST_TOOL,
                  AgentState.WAIT_CONFIRMATION):
            sm.transition(s)
        now_ms = int(self._clock() * 1000)
        verdict = self._confirmation.interpret(user_text, now_ms)
        if verdict == "no_pending":
            return TurnOutcome(state=AgentState.CANCELLED,
                               code="no_pending_confirmation")
        if verdict == "expired":
            self._confirmation.expire()
            self._audit.record("agent_confirmation_expired",
                               request_id=request_id,
                               stage="wait_confirmation",
                               code=CONFIRM_EXPIRED_CODE)
            return TurnOutcome(state=AgentState.TOOL_FAILED,
                               code=CONFIRM_EXPIRED_CODE)
        if verdict == "cancel":
            p = self._confirmation.cancel()
            self._audit.record("agent_confirmation_cancelled",
                               request_id=request_id,
                               stage="wait_confirmation",
                               tool=p.tool_name if p else "")
            return TurnOutcome(state=AgentState.CANCELLED,
                               answer_text="已取消，未做任何更改",
                               code="confirmation_cancelled")
        if verdict == "ask_again":
            return TurnOutcome(
                state=AgentState.WAIT_CONFIRMATION,
                answer_text="请明确回复：确认 或 取消",
                code="pending_confirmation")
        # accept
        p = self._confirmation.accept()
        if cancel_event.is_set():
            return TurnOutcome(state=AgentState.CANCELLED, code=CANCEL_CODE)
        self._audit.record("agent_confirmation_accepted",
                           request_id=request_id, stage="request_tool",
                           tool=p.tool_name)
        sm.transition(AgentState.REQUEST_TOOL)
        sm.transition(AgentState.WAIT_TOOL_RESULT)
        res = self._tools.execute(p.tool_name, p.normalized_args,
                                  p.tool_call_id)
        if not res.get("ok"):
            self._audit.record("agent_tool_result", request_id=request_id,
                               stage="observe_result", tool=p.tool_name,
                               code=res.get("code") or "tool_failed")
            return TurnOutcome(state=AgentState.TOOL_FAILED,
                               code=res.get("code") or "tool_failed")
        self._audit.record("agent_tool_result", request_id=request_id,
                           stage="observe_result", tool=p.tool_name,
                           code="ok")
        self._audit.record("agent_observation_applied",
                           request_id=request_id, stage="observe_result",
                           tool=p.tool_name)
        self._audit.record("agent_turn_completed", request_id=request_id,
                           stage="respond")
        return TurnOutcome(state=AgentState.COMPLETE,
                           answer_text="已执行：%s" % p.tool_name,
                           code="ok")
