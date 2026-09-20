"""test_r83_r4_router.py — R8_R4 §10.3 模型路由与工具白名单测试 (本地替身, 无公网)。"""
import json
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.router import (  # noqa: E402
    route_decision, detect_explicit_override,
    ROUTE_FAST, ROUTE_STRONG, ROUTE_LOCAL_LLM, ROUTE_LOCAL_TOOL)
from holopet_agentd.tools.whitelist import (  # noqa: E402
    validate_tool_calls, confirm_required, TOOL_WHITELIST)


class RouteDecision(unittest.TestCase):
    def test_simple_chat_to_fast(self):
        self.assertEqual(ROUTE_FAST, route_decision("你好呀", True)[0])
        self.assertEqual(ROUTE_FAST,
                         route_decision("今天天气怎么样？", True)[0])

    def test_complex_to_strong(self):
        self.assertEqual(ROUTE_STRONG,
                         route_decision("帮我分析这段代码的时间复杂度", True)[0])
        self.assertEqual(ROUTE_STRONG,
                         route_decision("证明这个数学定理并给出详细推导", True)[0])

    def test_explicit_overrides(self):
        self.assertEqual(ROUTE_FAST, route_decision("随便说点什么，请快速回答", True)[0])
        self.assertEqual(ROUTE_STRONG, route_decision("这个问题请深入分析", True)[0])
        self.assertEqual(ROUTE_LOCAL_LLM,
                         route_decision("本地处理一下这个", True)[0])

    def test_offline_to_local(self):
        self.assertEqual(ROUTE_LOCAL_LLM, route_decision("你好", False)[0])
        self.assertEqual(ROUTE_LOCAL_LLM,
                         route_decision("复杂分析问题", False)[0])

    def test_local_feature_fast(self):
        self.assertEqual(ROUTE_FAST,
                         route_decision("帮我设一个10分钟的定时器", True)[0])
        self.assertEqual(ROUTE_FAST,
                         route_decision("明早七点半叫我起床", True)[0])

    def test_long_text_to_strong(self):
        self.assertEqual(ROUTE_STRONG, route_decision("x" * 700, True)[0])

    def test_detect_override(self):
        self.assertEqual("deep", detect_explicit_override("请深入分析"))
        self.assertEqual("fast", detect_explicit_override("快速回答我"))
        self.assertEqual("local", detect_explicit_override("本地处理"))
        self.assertIsNone(detect_explicit_override("普通问题"))


class ToolWhitelist(unittest.TestCase):
    def _call(self, name, args, cid="c1"):
        return [{"id": cid, "function": {"name": name,
                                         "arguments": json.dumps(args)}}]

    def test_known_tool_accepted(self):
        acc, rej = validate_tool_calls(
            self._call("create_timer", {"duration_ms": "60000", "label": "tea"}))
        self.assertEqual(1, len(acc))
        self.assertEqual([], rej)
        self.assertEqual({"duration_ms": "60000", "label": "tea"},
                         acc[0]["arguments"])

    def test_unknown_tool_rejected(self):
        acc, rej = validate_tool_calls(self._call("sudo_rm", {}))
        self.assertEqual([], acc)
        self.assertEqual("unknown_tool", rej[0][0])

    def test_dangerous_name_rejected(self):
        acc, rej = validate_tool_calls(
            self._call("rm", {}))
        self.assertEqual([], acc)
        self.assertIn(rej[0][0], ("unknown_tool", "dangerous_name"))

    def test_missing_and_unexpected_args(self):
        acc, rej = validate_tool_calls(self._call("create_timer", {}))
        self.assertEqual([], acc)
        self.assertEqual("missing_args", rej[0][0])
        acc, rej = validate_tool_calls(
            self._call("show_clock", {"path": "/etc/passwd"}))
        self.assertEqual([], acc)
        self.assertEqual("unexpected_args", rej[0][0])

    def test_duplicate_call_id_once(self):
        calls = self._call("show_pet", {}) + self._call("show_pet", {})
        acc, rej = validate_tool_calls(calls)
        self.assertEqual(1, len(acc))
        self.assertEqual("duplicate_call_id", rej[0][0])

    def test_bad_arguments_json(self):
        acc, rej = validate_tool_calls(
            [{"id": "c1", "function": {"name": "show_pet", "arguments": "not json"}}])
        self.assertEqual([], acc)
        self.assertEqual("bad_arguments", rej[0][0])

    def test_confirm_required_for_delete_all(self):
        self.assertTrue(confirm_required("delete_note", {"id": "all"}))
        self.assertTrue(confirm_required("delete_alarm", {"id": "all"}))
        self.assertTrue(confirm_required("cancel_timer", {"id": "all"}))
        self.assertFalse(confirm_required("delete_note", {"id": "n1"}))

    def test_whitelist_exactly_section5(self):
        expected = {
            "create_timer", "list_timers", "cancel_timer",
            "create_alarm", "list_alarms", "enable_alarm", "delete_alarm",
            "show_clock", "show_pet",
            "add_note", "list_notes", "read_note", "delete_note",
            # R5_R4 (B2) 新增
            "get_ai_mode", "set_ai_mode", "dismiss_alert",
            "remember_preference", "list_preferences",
            "forget_preference",
        }
        self.assertEqual(expected, set(TOOL_WHITELIST.keys()))




class ToolRequestWire(unittest.TestCase):
    """R8_R4 B6: 白名单工具经 tool_request 走 UDS + 有界等待 tool_result。"""

    def _run_turn(self, tool_name, args_json, pre_result=None):
        import threading as _th
        from holopet_agentd import main as wm
        from holopet_agentd.providers import make_provider as _mp
        events = []

        class ToolChatProvider:
            def chat(self, messages):
                if not self.calls:
                    self.calls += 1
                    return {"content": "",
                            "tool_calls": [{"id": "c1",
                                            "function": {"name": tool_name,
                                                         "arguments": args_json}}]}
                return {"content": "好的，已处理", "tool_calls": []}

            def __init__(self):
                self.calls = 0

        orig = wm.make_provider
        wm.make_provider = lambda cfg: ToolChatProvider()
        cfg = {"provider": "fake", "text_only": True,
               "max_tool_rounds": 2}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t1", "帮我设一个定时器")
            if pre_result is not None:
                runner._tool_results = {("t1", "c1"): pre_result}
                import threading as _t2
                runner._tool_cond = _t2.Condition()
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        return events, runner

    def test_whitelist_tool_sends_tool_request_and_awaits(self):
        events, runner = self._run_turn(
            "show_clock", "{}",
            pre_result={"ok": True, "code": "", "result": {}})
        treq = [f for t, f in events if t == "tool_request"]
        self.assertEqual(1, len(treq), events)
        self.assertIn("show_clock", treq[0]["text"])
        self.assertIn("tool_call_id", treq[0]["text"])
        # 结果回灌: 下一轮 LLM 收到 tool 消息
        self.assertTrue(any(t == "tool_call" for t, _ in events))
        self.assertTrue(any(t == "content" for t, _ in events))

    def test_whitelist_tool_confirm_required_blocked(self):
        events, _ = self._run_turn(
            "delete_note", '{"id": "all"}',
            pre_result=None)
        treq = [f for t, f in events if t == "tool_request"]
        self.assertEqual(0, len(treq), "危险模糊命令不得发 tool_request")

    def test_non_whitelist_tool_keeps_local_dispatch(self):
        events, _ = self._run_turn("time", "{}", pre_result=None)
        treq = [f for t, f in events if t == "tool_request"]
        self.assertEqual(0, len(treq), "内部工具不走 UDS")




class ProviderRouting(unittest.TestCase):
    """R8_R4: 三路 provider 选择 (快/强/本地) 与指标记账。"""

    def _runner(self, cfg_extra=None):
        from holopet_agentd import main as wm
        cfg = {"provider": "fake", "text_only": True}
        cfg.update(cfg_extra or {})
        return wm.TurnRunner(cfg)

    def test_local_llm_unavailable_is_stable_error(self):
        import threading
        from holopet_agentd import main as wm
        os.environ["HOLOPET_OFFLINE"] = "1"
        try:
            runner = self._runner()
            runner.set_transcript("t1", "你好")
            events = []
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
            errs = [f for tt, f in events if tt == "error"]
            self.assertTrue(any(e.get("code") == "local_llm_unavailable"
                                for e in errs), events)
        finally:
            os.environ.pop("HOLOPET_OFFLINE", None)

    def test_route_metrics_recorded(self):
        import threading
        from holopet_agentd import main as wm
        orig = wm.make_provider

        class P:
            def chat(self, messages):
                return {"content": "你好！", "tool_calls": []}

        wm.make_provider = lambda cfg: P()
        try:
            runner = self._runner({"fast_llm": {"provider": "fake"},
                                   "strong_llm": {"provider": "fake"}})
            runner.set_transcript("t1", "你好呀，简单回答")
            runner.run("t1", threading.Event(), lambda *a, **k: True)
            self.assertEqual("fast", runner._turn_metrics.get("ROUTE"))
            self.assertIn(runner._turn_metrics.get("ROUTE_REASON"),
                          ("explicit_fast", "short_greeting"))
        finally:
            wm.make_provider = orig

    def test_model_metric_and_llm_latency_recorded(self):
        """R8_R4_R3_R4_R5_R3 (B): 每轮记账请求模型名 + LLM 应答延迟。"""
        import threading
        from holopet_agentd import main as wm
        orig = wm.make_provider

        class P:
            def chat(self, messages):
                return {"content": "好的", "tool_calls": []}

        wm.make_provider = lambda cfg: P()
        try:
            runner = self._runner(
                {"fast_llm": {"provider": "fake",
                              "model": "deepseek-v4-flash"}})
            runner.set_transcript("t1", "你好呀")
            runner.run("t1", threading.Event(), lambda *a, **k: True)
            self.assertEqual("deepseek-v4-flash",
                             runner._turn_metrics.get("MODEL"))
            self.assertIn("LLM_ANSWER_MS", runner._turn_metrics)
            self.assertGreaterEqual(
                int(runner._turn_metrics.get("LLM_ANSWER_MS", -1)), 0)
        finally:
            wm.make_provider = orig

    def test_broken_fast_provider_no_silent_substitution(self):
        """R8_R4_R3_R4_R5_R3 (B): fast provider 不可用 → PROVIDER_MODEL_BLOCKED,
        绝不静默回落主模型。"""
        import threading
        from holopet_agentd import main as wm
        orig = wm.make_provider

        class MainP:
            def chat(self, messages):
                return {"content": "不应被调用", "tool_calls": []}

        def _mk(cfg):
            if cfg.get("__fast"):
                raise RuntimeError("broken fast")
            return MainP()

        wm.make_provider = _mk
        try:
            runner = self._runner(
                {"fast_llm": {"provider": "fake", "__fast": True,
                              "model": "deepseek-v4-flash"}})
            runner.set_transcript("t1", "你好呀")
            events = []
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
            errs = [f for tt, f in events if tt == "error"]
            self.assertTrue(any(e.get("code") == "PROVIDER_MODEL_BLOCKED"
                                for e in errs), events)
            self.assertFalse(any(tt == "content" for tt, _ in events),
                             "不得静默换模型出内容")
        finally:
            wm.make_provider = orig

    def test_broken_strong_provider_no_silent_substitution(self):
        import threading
        from holopet_agentd import main as wm
        orig = wm.make_provider

        class MainP:
            def chat(self, messages):
                return {"content": "不应被调用", "tool_calls": []}

        def _mk(cfg):
            if cfg.get("__strong"):
                raise RuntimeError("broken strong")
            return MainP()

        wm.make_provider = _mk
        try:
            runner = self._runner(
                {"strong_llm": {"provider": "fake", "__strong": True,
                                "model": "deepseek-v4-pro"}})
            runner.set_transcript("t1", "帮我深入分析这段代码")
            events = []
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
            errs = [f for tt, f in events if tt == "error"]
            self.assertTrue(any(e.get("code") == "PROVIDER_MODEL_BLOCKED"
                                for e in errs), events)
        finally:
            wm.make_provider = orig


if __name__ == "__main__":
    unittest.main(verbosity=2)
