# -*- coding: utf-8 -*-
"""test_r5_r4_agent_runtime.py — R5_R4 工作包 A/F: AgentRuntime 定向测试
(Fake 模型 + Fake 工具执行器; 覆盖多步骤/失败阻断/确认/预算/取消/升级/单一终态)。
"""
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.agent import (  # noqa: E402
    AgentRuntime, AgentState, TurnOutcome)
from holopet_agentd.providers.errors import NetworkError  # noqa: E402

SYS = "系统提示词"


def _tc(name, args, cid="c1"):
    return [{"id": cid, "function": {"name": name, "arguments": args}}]


class ScriptedModel:
    """按脚本依次返回响应; 用完重复最后一个。"""

    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = 0

    def chat(self, messages):
        self.calls += 1
        if self.responses:
            return self.responses.pop(0)
        return {"content": "默认答复", "tool_calls": []}


class FakeTools:
    """内存工具执行器: 记录执行、可控失败、确认判定。"""

    def __init__(self, confirm_for=(), fail=(), side_effect=()):
        self.executed = []
        self.confirm_for = set(confirm_for)
        self.fail = set(fail)
        # 默认: 已知读取类工具无副作用, 其余有
        self.readonly = {"get_time", "get_status", "list_timers",
                         "list_alarms", "list_notes", "read_note",
                         "show_clock", "show_pet", "get_ai_mode",
                         "list_preferences"}
        self.side_effect = set(side_effect)

    def validate(self, tool_calls):
        ok, rej = [], []
        for c in tool_calls:
            fn = c.get("function") or {}
            if not fn.get("name"):
                rej.append(("", "bad_call"))
            else:
                ok.append(c)
        return ok, rej

    def needs_confirmation(self, tool_name, arguments):
        return (tool_name in self.confirm_for
                and arguments.get("id") == "all")

    def has_side_effect(self, tool_name):
        return tool_name not in self.readonly or tool_name in self.side_effect

    def execute(self, tool_name, arguments, call_id):
        self.executed.append((tool_name, arguments, call_id))
        if tool_name in self.fail:
            return {"ok": False, "code": "tool_timeout", "result": {}}
        return {"ok": True, "code": "", "result": {"id": "t1"}}


def make_runtime(model, tools, **kw):
    return AgentRuntime(model_client=model, tool_executor=tools, **kw)


class RuntimeTests(unittest.TestCase):
    def test_single_tool_happy_path(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("get_time", {})},
            {"content": "现在是下午三点。", "tool_calls": []},
        ])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="现在几点",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertEqual("现在是下午三点。", out.answer_text)
        self.assertEqual([("get_time", {}, "c1")], t.executed)

    def test_multi_step_two_tools(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("create_timer",
                                              {"duration_ms": 20000})},
            {"content": "", "tool_calls": _tc("add_note", {"text": "x"},
                                              cid="c2")},
            {"content": "都办好了。", "tool_calls": []},
        ])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="定时并记便签",
                         model_alias="strong", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertEqual(2, len(t.executed))

    def test_tool_failure_blocks_chain_and_answers(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("create_timer",
                                              {"duration_ms": 1})},
            {"content": "抱歉，定时器设置失败。", "tool_calls": []},
        ])
        t = FakeTools(fail={"create_timer"})
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="定时",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertIn("失败", out.answer_text)

    def test_tool_after_failure_blocked(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("create_timer",
                                              {"duration_ms": 1})},
            {"content": "", "tool_calls": _tc("add_note", {"text": "x"},
                                              cid="c2")},
        ])
        t = FakeTools(fail={"create_timer"})
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="定时",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.TOOL_FAILED, out.state)
        self.assertEqual("tool_chain_blocked", out.code)
        self.assertEqual(1, len(t.executed), "失败后不得继续执行依赖工具")

    def test_confirmation_flow_accept_once(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("delete_note",
                                              {"id": "all"})},
        ])
        t = FakeTools(confirm_for={"delete_note"})
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="删除全部便签",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.WAIT_CONFIRMATION, out.state)
        self.assertEqual([], t.executed, "首轮不得执行副作用")
        # 用户确认
        out2 = r.handle_confirmation(request_id="r1", user_text="确认",
                                     model_alias="fast",
                                     cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out2.state)
        self.assertEqual(1, len(t.executed), "确认后恰好执行一次")
        self.assertEqual(("delete_note", {"id": "all"}, "c1"),
                         t.executed[0])

    def test_confirmation_cancel_no_side_effect(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("delete_note",
                                              {"id": "all"})},
        ])
        t = FakeTools(confirm_for={"delete_note"})
        r = make_runtime(m, t)
        r.run_turn(request_id="r1", user_text="删除全部便签",
                   model_alias="fast", system_text=SYS,
                   cancel_event=threading.Event())
        out = r.handle_confirmation(request_id="r1", user_text="取消",
                                    model_alias="fast",
                                    cancel_event=threading.Event())
        self.assertEqual(AgentState.CANCELLED, out.state)
        self.assertEqual([], t.executed)

    def test_confirmation_expired(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("delete_note",
                                              {"id": "all"})},
        ])
        t = FakeTools(confirm_for={"delete_note"})
        fake = [1000.0]
        r = AgentRuntime(model_client=m, tool_executor=t,
                         clock=lambda: fake[0])
        r.run_turn(request_id="r1", user_text="删除全部",
                   model_alias="fast", system_text=SYS,
                   cancel_event=threading.Event())
        fake[0] += 31.0                      # 30s TTL 已过
        out = r.handle_confirmation(request_id="r1", user_text="确认",
                                    model_alias="fast",
                                    cancel_event=threading.Event())
        self.assertEqual(AgentState.TOOL_FAILED, out.state)
        self.assertEqual("confirmation_expired", out.code)
        self.assertEqual([], t.executed)

    def test_budget_exceeded(self):
        # 2 轮 × 每轮 4 工具 = 8 (总量上限); 第 3 轮第 1 个工具超限
        m = ScriptedModel([
            {"content": "",
             "tool_calls": [_tc("get_time", {}, cid="a%d" % i)[0]
                            for i in range(4)]},
            {"content": "",
             "tool_calls": [_tc("get_status", {}, cid="b%d" % i)[0]
                            for i in range(4)]},
            {"content": "",
             "tool_calls": _tc("get_time", {}, cid="c9")},
        ])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="x",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.TOOL_FAILED, out.state)
        self.assertEqual("agent_budget_exceeded", out.code)
        self.assertEqual(8, len(t.executed))

    def test_cancel_mid_turn(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("get_time", {})},
            {"content": "之后不该出现", "tool_calls": []},
        ])
        t = FakeTools()
        ev = threading.Event()

        class CancellingTools(FakeTools):
            def execute(self, name, args, cid):
                ev.set()                     # 工具执行瞬间触发取消
                return super().execute(name, args, cid)

        r = make_runtime(m, CancellingTools())
        out = r.run_turn(request_id="r1", user_text="x",
                         model_alias="fast", system_text=SYS,
                         cancel_event=ev)
        self.assertEqual(AgentState.CANCELLED, out.state)

    def test_model_error_stable_code(self):
        class Broken:
            def chat(self, messages):
                raise NetworkError("x")

        t = FakeTools()
        r = make_runtime(Broken(), t)
        out = r.run_turn(request_id="r1", user_text="x",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.MODEL_FAILED, out.state)
        self.assertEqual("network_error", out.code)

    def test_format_fixup_at_most_once(self):
        m = ScriptedModel([
            {"content": 42, "tool_calls": "not-a-list"},
            {"content": 42, "tool_calls": "not-a-list"},
        ])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="x",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.MODEL_FAILED, out.state)
        self.assertEqual("model_format_error", out.code)
        self.assertEqual(2, m.calls, "修复请求最多一次")

    def test_escalate_at_most_once(self):
        fast = ScriptedModel([
            {"content": "能力不足，需要更强的模型", "tool_calls": []},
        ])
        strong = ScriptedModel([
            {"content": "深入分析后的完整答案", "tool_calls": []},
        ])
        r = AgentRuntime(model_client=fast, tool_executor=FakeTools(),
                         escalate_cb=lambda: strong,
                         insufficient_markers=("能力不足",))
        out = r.run_turn(request_id="r1", user_text="复杂问题",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertIn("深入分析", out.answer_text)
        self.assertEqual(1, fast.calls)
        self.assertEqual(1, strong.calls)

    def test_empty_answer_code(self):
        m = ScriptedModel([{"content": "", "tool_calls": []}])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="x",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertEqual("empty_answer", out.code)

    def test_audit_no_body_leak(self):
        m = ScriptedModel([
            {"content": "", "tool_calls": _tc("get_time", {})},
            {"content": "秘密正文 XYZ", "tool_calls": []},
        ])
        t = FakeTools()
        r = make_runtime(m, t)
        r.run_turn(request_id="r1", user_text="秘密问题 XYZ",
                   model_alias="fast", system_text=SYS,
                   cancel_event=threading.Event())
        blob = str(r._audit.events)
        self.assertNotIn("秘密问题 XYZ", blob)
        self.assertNotIn("秘密正文 XYZ", blob)


    def test_deterministic_fast_path_skips_model(self):
        m = ScriptedModel([{"content": "不该出现", "tool_calls": []}])
        t = FakeTools()
        r = make_runtime(m, t)
        out = r.run_turn(request_id="r1", user_text="显示时钟",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.COMPLETE, out.state)
        self.assertEqual(0, m.calls, "确定性快速路径不得调用模型")
        self.assertEqual([("show_clock", {}, t.executed[0][2])],
                         [(n, a, c) for n, a, c in t.executed])

    def test_deterministic_fast_path_answers(self):
        r = make_runtime(ScriptedModel([]), FakeTools())
        for phrase, tool in (("返回宠物", "show_pet"), ("停止响铃",
                                                       "dismiss_alert"),
                             ("查询当前 AI 模式", "get_ai_mode")):
            t = FakeTools()
            rt = make_runtime(ScriptedModel([]), t)
            out = rt.run_turn(request_id="r2", user_text=phrase,
                              model_alias="fast", system_text=SYS,
                              cancel_event=threading.Event())
            self.assertEqual(AgentState.COMPLETE, out.state, phrase)
            self.assertEqual(tool, t.executed[0][0], phrase)

    def test_deterministic_fast_not_matched_for_numbers(self):
        self.assertIsNone(AgentRuntime._deterministic_fast("设置一个3分钟的定时器"))
        self.assertIsNone(AgentRuntime._deterministic_fast("删除全部便签"))
        self.assertIsNone(AgentRuntime._deterministic_fast("显示时钟和闹钟"))

    def test_deterministic_fast_failure_stable(self):
        t = FakeTools(fail={"show_clock"})
        r = make_runtime(ScriptedModel([]), t)
        out = r.run_turn(request_id="r1", user_text="显示时钟",
                         model_alias="fast", system_text=SYS,
                         cancel_event=threading.Event())
        self.assertEqual(AgentState.TOOL_FAILED, out.state)
        self.assertEqual("tool_timeout", out.code)

    def test_scenario_table(self):
        """R5_R4 (F): 脚本化确定性场景表 (Fake 模型) — 单工具/多步骤/
        确认/失败/无工具, 全部核对执行序列与终态。"""
        T = FakeTools
        cases = [
            # (responses, tools_kw, user_text, want_state, want_code, want_exec)
            ([{"content": "", "tool_calls": _tc("create_alarm",
                                                {"hour": 7, "minute": 30})},
              {"content": "闹钟设好了", "tool_calls": []}],
             {}, "明早七点半叫我起床", AgentState.COMPLETE, "",
             ["create_alarm"]),
            ([{"content": "", "tool_calls": _tc("add_note",
                                                {"text": "测试"})},
              {"content": "记好了", "tool_calls": []}],
             {}, "记一条便签", AgentState.COMPLETE, "",
             ["add_note"]),
            ([{"content": "", "tool_calls": _tc("delete_note",
                                                {"id": "n1"})},
              {"content": "已删除", "tool_calls": []}],
             {}, "删除那条便签", AgentState.COMPLETE, "",
             ["delete_note"]),
            ([{"content": "", "tool_calls": _tc("enable_alarm",
                                                {"id": "a1", "on": True})},
              {"content": "已启用", "tool_calls": []}],
             {}, "启用闹钟", AgentState.COMPLETE, "",
             ["enable_alarm"]),
            ([{"content": "", "tool_calls": _tc("set_ai_mode",
                                                {"mode": "fast"})},
              {"content": "已切换", "tool_calls": []}],
             {}, "把 AI 模式改成快速", AgentState.COMPLETE, "",
             ["set_ai_mode"]),
            ([{"content": "", "tool_calls": _tc("dismiss_alert", {})},
              {"content": "好的", "tool_calls": []}],
             {}, "停止响铃", AgentState.COMPLETE, "",
             ["dismiss_alert"]),
            ([{"content": "", "tool_calls": _tc("get_ai_mode", {})},
              {"content": "当前是自动模式", "tool_calls": []}],
             {}, "现在是什么 AI 模式", AgentState.COMPLETE, "",
             ["get_ai_mode"]),
            ([{"content": "", "tool_calls": _tc("create_timer",
                                                {"duration_ms": 20000})},
              {"content": "", "tool_calls": _tc("add_note",
                                                {"text": "测试喇叭"},
                                                cid="c2")},
              {"content": "完成", "tool_calls": []}],
             {}, "设置一个20秒定时器再记一条便签", AgentState.COMPLETE, "",
             ["create_timer", "add_note"]),
            ([{"content": "深入分析结果", "tool_calls": []}],
             {}, "解释一下相对论", AgentState.COMPLETE, "",
             []),
            ([{"content": "", "tool_calls": _tc("delete_note",
                                                {"id": "all"})}],
             {"confirm_for": {"delete_note"}},
             "删除全部便签", AgentState.WAIT_CONFIRMATION,
             "pending_confirmation", []),
            ([{"content": "", "tool_calls": _tc("cancel_timer",
                                                {"id": "all"})}],
             {"confirm_for": {"cancel_timer"}},
             "取消全部定时器", AgentState.WAIT_CONFIRMATION,
             "pending_confirmation", []),
        ]
        for i, (resp, tkw, text, want_state, want_code,
                want_exec) in enumerate(cases):
            with self.subTest(i=i, text=text):
                m = ScriptedModel(resp)
                t = T(**tkw)
                r = make_runtime(m, t)
                out = r.run_turn(request_id="r%d" % i, user_text=text,
                                 model_alias="fast", system_text=SYS,
                                 cancel_event=threading.Event())
                self.assertEqual(want_state, out.state, text)
                if want_code:
                    self.assertEqual(want_code, out.code, text)
                self.assertEqual(want_exec, [x[0] for x in t.executed],
                                 text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
