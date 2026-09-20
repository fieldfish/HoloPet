"""test_r83_r4_r2_flow.py — R8_R4_R2 §7.2/§7.3 流程测试 (本地替身, 无公网)。

覆盖以下端到端流程:
  fast_escalation -> strong exactly once (无工具重放, strong 失败不回退)
  waiting_feedback -> visible before slow answer (strong 立即; fast 软等待)
  detect_insufficient 纯函数; protocol "waiting" 类型进表
"""
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.router import detect_insufficient  # noqa: E402


class DetectInsufficient(unittest.TestCase):
    def test_marker_hit(self):
        self.assertTrue(detect_insufficient("抱歉，这个问题超出我的能力范围"))
        self.assertTrue(detect_insufficient("我无法回答这个问题"))

    def test_no_marker(self):
        self.assertFalse(detect_insufficient("今天是晴天，适合出门散步。"))
        self.assertFalse(detect_insufficient(""))
        self.assertFalse(detect_insufficient(None))

    def test_custom_markers(self):
        self.assertTrue(detect_insufficient("我不行", markers=("我不行",)))
        self.assertFalse(detect_insufficient("我不行", markers=("别",)))


class Escalation(unittest.TestCase):
    """fast 回答"能力不足" → strong 恰好一次; 不重放工具; 不回退 fast。"""

    def _run(self, fast_content, strong_content, strong_error=None):
        from holopet_agentd import main as wm
        events = []
        calls = {"fast": 0, "strong": 0}

        class Fast:
            def chat(self, messages):
                calls["fast"] += 1
                return {"content": fast_content, "tool_calls": []}

        class Strong:
            def __init__(self):
                self.error = strong_error

            def chat(self, messages):
                calls["strong"] += 1
                if self.error:
                    raise self.error
                return {"content": strong_content, "tool_calls": []}

        orig = wm.make_provider
        wm.make_provider = lambda cfg: (Strong()
                                        if cfg.get("__strong") else Fast())
        cfg = {"provider": "fake", "text_only": True,
               "strong_llm": {"provider": "fake", "__strong": True}}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t1", "你好，请简单回答")
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        return events, calls, runner

    def test_fast_insufficient_escalates_once(self):
        events, calls, runner = self._run(
            "这个问题超出我的能力", "这是深入分析后的完整答案。")
        self.assertEqual(1, calls["fast"], calls)
        self.assertEqual(1, calls["strong"], calls)
        contents = [f for t, f in events if t == "content"]
        self.assertEqual(1, len(contents))
        self.assertIn("深入分析", contents[0]["text"])
        self.assertEqual("strong", runner._turn_metrics.get("ROUTE"))
        self.assertEqual("fast", runner._turn_metrics.get("ROUTE_ORIGINAL"))
        self.assertEqual("fast_to_strong",
                         runner._turn_metrics.get("ESCALATED"))
        # 无 tool 重放: 事件流中不得出现 tool_request/tool_call
        self.assertFalse(any(t == "tool_request" for t, _ in events))
        self.assertFalse(any(t == "tool_call" for t, _ in events))

    def test_no_escalation_when_fast_capable(self):
        events, calls, runner = self._run("好的，没问题。", "不应调用")
        self.assertEqual(1, calls["fast"])
        self.assertEqual(0, calls["strong"])
        self.assertNotIn("ESCALATED", runner._turn_metrics)
        self.assertEqual("fast", runner._turn_metrics.get("ROUTE"))

    def test_strong_failure_gives_error_no_fallback(self):
        from holopet_agentd.providers.errors import NetworkError
        events, calls, _ = self._run(
            "我无法回答", "", strong_error=NetworkError("x"))
        self.assertEqual(1, calls["fast"])
        self.assertEqual(1, calls["strong"])
        errs = [f for t, f in events if t == "error"]
        self.assertEqual(1, len(errs), events)
        self.assertTrue(any(f.get("code", "").startswith("llm_")
                            for f in errs), errs)


class WaitingFeedback(unittest.TestCase):
    """§7.3: strong 立即 waiting; fast 软等待超时后 waiting; 首段内容解除。"""

    def test_strong_route_waiting_immediate(self):
        import time as _t
        from holopet_agentd import main as wm
        events = []

        class SlowStrong:
            def chat(self, messages):
                _t.sleep(0.05)
                return {"content": "答案", "tool_calls": []}

        orig = wm.make_provider
        wm.make_provider = lambda cfg: SlowStrong()
        cfg = {"provider": "fake", "text_only": True,
               "strong_llm": {"provider": "fake"}}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t1", "帮我分析这段代码的时间复杂度")
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        waitings = [f for t, f in events if t == "waiting"]
        self.assertEqual(1, len(waitings), events)
        self.assertIn("多想一会儿", waitings[0]["text"])
        self.assertIn("ROUTE_TO_WAITING_MS", runner._turn_metrics)
        # waiting 必须早于首段 content
        idx_w = [i for i, (t, _) in enumerate(events) if t == "waiting"][0]
        idx_c = [i for i, (t, _) in enumerate(events) if t == "content"][0]
        self.assertLess(idx_w, idx_c)

    def test_fast_route_soft_wait_then_disarm(self):
        import time as _t
        from holopet_agentd import main as wm
        events = []

        class SlowFast:
            def chat(self, messages):
                _t.sleep(0.25)
                return {"content": "普通回答", "tool_calls": []}

        orig = wm.make_provider
        wm.make_provider = lambda cfg: SlowFast()
        cfg = {"provider": "fake", "text_only": True,
               "soft_wait_ms": 50, "waiting_text_soft": "正在回答，请稍候"}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t1", "你好呀")
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        waitings = [f for t, f in events if t == "waiting"]
        self.assertEqual(1, len(waitings), events)
        self.assertEqual("正在回答，请稍候", waitings[0]["text"])
        idx_w = [i for i, (t, _) in enumerate(events) if t == "waiting"][0]
        idx_c = [i for i, (t, _) in enumerate(events) if t == "content"][0]
        self.assertLess(idx_w, idx_c)

    def test_fast_route_fast_answer_no_waiting(self):
        import time as _t
        from holopet_agentd import main as wm
        events = []

        class QuickFast:
            def chat(self, messages):
                return {"content": "快答", "tool_calls": []}

        orig = wm.make_provider
        wm.make_provider = lambda cfg: QuickFast()
        cfg = {"provider": "fake", "text_only": True, "soft_wait_ms": 50}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t1", "你好呀")
            runner.run("t1", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        _t.sleep(0.15)   # 等软等待计时器窗口过去 (已被 disarm)
        waitings = [f for t, f in events if t == "waiting"]
        self.assertEqual(0, len(waitings), events)


class AiModeBinding(unittest.TestCase):
    """R8_R4_R3_R4: 菜单 AI 模式经 set_ai_mode 真实绑定 router。"""

    def _run_with_mode(self, mode, fast_content="fast答", strong_content="strong答"):
        import threading
        from holopet_agentd import main as wm
        calls = {"fast": 0, "strong": 0, "local": 0}
        events = []

        class F:
            def chat(self, messages):
                calls["fast"] += 1
                return {"content": fast_content, "tool_calls": []}

        class S:
            def chat(self, messages):
                calls["strong"] += 1
                return {"content": strong_content, "tool_calls": []}

        class L:
            def chat(self, messages):
                calls["local"] += 1
                return {"content": "本地答", "tool_calls": []}

        orig = wm.make_provider

        def mk(cfg):
            tag = cfg.get("__tag")
            if tag == "strong":
                return S()
            if tag == "local":
                return L()
            return F()

        wm.make_provider = mk
        cfg = {"provider": "fake", "text_only": True,
               "strong_llm": {"provider": "fake", "__tag": "strong"},
               "local_llm": {"provider": "fake", "__tag": "local"}}
        try:
            runner = wm.TurnRunner(cfg)
            runner.set_transcript("t", "你好呀")
            runner.set_ai_mode(mode)
            runner.run("t", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        return calls, events, runner

    def test_fast_mode_forces_fast(self):
        calls, _, runner = self._run_with_mode("fast")
        self.assertEqual(1, calls["fast"])
        self.assertEqual(0, calls["strong"])
        self.assertEqual("fast", runner._turn_metrics.get("ROUTE"))

    def test_deep_mode_forces_strong(self):
        calls, _, runner = self._run_with_mode("deep")
        self.assertEqual(0, calls["fast"])
        self.assertEqual(1, calls["strong"])
        self.assertEqual("strong", runner._turn_metrics.get("ROUTE"))

    def test_local_mode_forces_local(self):
        calls, _, runner = self._run_with_mode("local")
        self.assertEqual(1, calls["local"])
        self.assertEqual("local_llm", runner._turn_metrics.get("ROUTE"))

    def test_auto_mode_keeps_routing(self):
        calls, _, runner = self._run_with_mode("auto")
        self.assertEqual(1, calls["fast"])
        self.assertEqual("fast", runner._turn_metrics.get("ROUTE"))

    def test_local_mode_unavailable_readable_error(self):
        import threading
        from holopet_agentd import main as wm
        events = []
        orig = wm.make_provider
        wm.make_provider = lambda cfg: None  # local_llm 段构造失败 → 无本地
        cfg = {"provider": "fake", "text_only": True,
               "local_llm": {"provider": "fake"}}
        try:
            runner = wm.TurnRunner(cfg)
            runner._local_provider = None     # 显式无本地
            runner.set_transcript("t", "你好呀")
            runner.set_ai_mode("local")
            runner.run("t", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        errs = [f for t, f in events if t == "error"]
        self.assertEqual(1, len(errs), events)
        self.assertEqual("local_llm_unavailable", errs[0].get("code"))
        self.assertEqual("本地模型未就绪", errs[0].get("text"))


class RealOfflineDetection(unittest.TestCase):
    """R8_R4_R3_R4_R3: 真实断网 (无环境变量) 自动切 local_llm。"""

    def test_unreachable_host_detected_offline_routes_local(self):
        import threading
        from holopet_agentd import main as wm
        events = []

        class Local:
            def chat(self, messages):
                return {"content": "本地答", "tool_calls": []}

        orig = wm.make_provider
        wm.make_provider = lambda cfg: Local()
        cfg = {"provider": "fake",
               "base_url": "https://unreachable.invalid/v1",
               "fast_llm": {"provider": "fake",
                            "base_url": "https://unreachable.invalid/v1"},
               "local_llm": {"provider": "fake"},
               "text_only": True}
        os.environ.pop("HOLOPET_OFFLINE", None)
        try:
            runner = wm.TurnRunner(cfg)
            self.assertFalse(runner._online(), "不可达主机 → 判定离线")
            runner.set_transcript("t", "你好呀")
            runner.run("t", threading.Event(),
                       lambda r, tt, **f: (events.append((tt, f)), True)[1])
        finally:
            wm.make_provider = orig
        self.assertEqual("local_llm", runner._turn_metrics.get("ROUTE"))
        contents = [f for t, f in events if t == "content"]
        self.assertEqual("本地答", contents[0]["text"])

    def test_env_offline_still_wins(self):
        from holopet_agentd import main as wm
        os.environ["HOLOPET_OFFLINE"] = "1"
        try:
            runner = wm.TurnRunner({"provider": "fake"})
            self.assertFalse(runner._online())
        finally:
            os.environ.pop("HOLOPET_OFFLINE", None)


class ProtocolWaiting(unittest.TestCase):
    def test_waiting_in_known_and_event_types(self):
        from holopet_agentd.protocol import KNOWN_TYPES, EVENT_TYPES
        self.assertIn("waiting", KNOWN_TYPES)
        self.assertIn("waiting", EVENT_TYPES)

    def test_waiting_roundtrip(self):
        from holopet_agentd.protocol import parse_line
        m = parse_line('{"v":"1","request_id":"r1","type":"waiting",'
                       '"text":"这个问题要多想一会儿"}')
        self.assertEqual("waiting", m["type"])
        self.assertEqual("这个问题要多想一会儿", m["text"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
