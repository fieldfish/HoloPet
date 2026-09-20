# -*- coding: utf-8 -*-
"""test_r5_r4_agent_core.py — R5_R4 工作包 A 定向测试:
状态机合法性 / 预算 / 审计事件 / 确认状态机 / SQLite 持久层。
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.agent import (  # noqa: E402
    AgentAudit, AgentBudgets, AgentState, AgentStateMachine, AgentStore,
    BudgetExceeded, ConfirmationGate, IllegalTransition,
)


class StateMachineTests(unittest.TestCase):
    def test_legal_happy_path(self):
        sm = AgentStateMachine()
        for s in (AgentState.ACCEPT_INPUT, AgentState.ROUTE, AgentState.THINK,
                  AgentState.REQUEST_TOOL, AgentState.WAIT_TOOL_RESULT,
                  AgentState.OBSERVE_RESULT, AgentState.THINK_NEXT,
                  AgentState.RESPOND, AgentState.COMPLETE):
            sm.transition(s)
        self.assertTrue(sm.terminal_seen)

    def test_illegal_transition_rejected(self):
        sm = AgentStateMachine()
        with self.assertRaises(IllegalTransition):
            sm.transition(AgentState.THINK)          # IDLE → THINK 非法

    def test_terminal_is_sticky(self):
        sm = AgentStateMachine()
        sm.transition(AgentState.CANCELLED)
        with self.assertRaises(IllegalTransition):
            sm.transition(AgentState.ROUTE)

    def test_confirmation_bypass_chain(self):
        sm = AgentStateMachine()
        for s in (AgentState.ACCEPT_INPUT, AgentState.ROUTE, AgentState.THINK,
                  AgentState.REQUEST_TOOL, AgentState.WAIT_CONFIRMATION,
                  AgentState.REQUEST_TOOL, AgentState.WAIT_TOOL_RESULT,
                  AgentState.OBSERVE_RESULT, AgentState.RESPOND,
                  AgentState.COMPLETE):
            sm.transition(s)

    def test_fail_paths_legal(self):
        for fail, path in (
            (AgentState.MODEL_FAILED, (AgentState.ACCEPT_INPUT,
                                       AgentState.ROUTE, AgentState.THINK)),
            (AgentState.TIMEOUT, (AgentState.ACCEPT_INPUT,
                                  AgentState.ROUTE, AgentState.THINK)),
            (AgentState.TOOL_FAILED, (AgentState.ACCEPT_INPUT,
                                      AgentState.ROUTE, AgentState.THINK,
                                      AgentState.REQUEST_TOOL)),
        ):
            sm = AgentStateMachine()
            for s in path:
                sm.transition(s)
            sm.transition(fail)
            sm.transition(AgentState.COMPLETE)

    def test_reset(self):
        sm = AgentStateMachine()
        sm.transition(AgentState.CANCELLED)
        sm.reset()
        self.assertEqual(AgentState.IDLE, sm.state)
        self.assertFalse(sm.terminal_seen)


class BudgetTests(unittest.TestCase):
    def test_rounds_cap(self):
        b = AgentBudgets(max_rounds=4)
        for _ in range(4):
            b.begin_round()
        with self.assertRaises(BudgetExceeded) as ctx:
            b.begin_round()
        self.assertEqual("agent_budget_exceeded", ctx.exception.code)

    def test_per_round_cap(self):
        b = AgentBudgets(max_per_round=4)
        b.begin_round()
        for _ in range(4):
            b.charge_tool("t")
        with self.assertRaises(BudgetExceeded):
            b.charge_tool("t")

    def test_total_cap(self):
        b = AgentBudgets(max_total=8, max_rounds=4, max_per_round=4)
        for _ in range(2):
            b.begin_round()
            for _ in range(4):
                b.charge_tool("t")
        b.begin_round()
        with self.assertRaises(BudgetExceeded):
            b.charge_tool("t")


class AuditTests(unittest.TestCase):
    def test_event_recorded_without_body(self):
        a = AgentAudit()
        a.record("agent_tool_requested", request_id="r1", stage="request_tool",
                 tool="create_timer")
        a.record("agent_turn_completed", request_id="r1", stage="respond",
                 model="deepseek-v4-flash",
                 content_text="这是一段正文，不得进入审计")
        evs = a.events
        self.assertEqual(2, len(evs))
        self.assertNotIn("这是一段正文", str(evs))
        self.assertIn("content_sha256", evs[1])
        self.assertNotIn("content", str(evs[1]["content_chars"]))

    def test_unknown_event_rejected(self):
        a = AgentAudit()
        with self.assertRaises(ValueError):
            a.record("agent_hacked", request_id="r", stage="x")

    def test_summary_counts(self):
        a = AgentAudit()
        for _ in range(3):
            a.record("agent_tool_result", request_id="r", stage="observe",
                     tool="get_time")
        self.assertEqual({"agent_tool_result": 3}, a.summary())


class ConfirmationTests(unittest.TestCase):
    def test_accept_and_cancel_words(self):
        g = ConfirmationGate(ttl_seconds=30)
        p = g.request(request_id="r1", tool_name="delete_note",
                      normalized_args={"id": "all"}, raw_request={"x": 1},
                      tool_call_id="c1")
        now = p.created_ms + 1000
        self.assertEqual("accept", g.interpret("确认", now))
        g2 = ConfirmationGate()
        p2 = g2.request(request_id="r1", tool_name="delete_note",
                        normalized_args={"id": "all"}, raw_request={},
                        tool_call_id="c1")
        self.assertEqual("cancel", g2.interpret("算了", p2.created_ms + 1))

    def test_other_input_asks_again_without_extending(self):
        g = ConfirmationGate(ttl_seconds=30)
        p = g.request(request_id="r1", tool_name="delete_note",
                      normalized_args={"id": "all"}, raw_request={},
                      tool_call_id="c1")
        self.assertEqual("ask_again", g.interpret("天气不错", p.created_ms))
        # 过期不受 ask_again 影响 (不延长)
        late = p.expires_ms + 1
        self.assertEqual("expired", g.interpret("天气不错", late))

    def test_expire_returns_code(self):
        g = ConfirmationGate(ttl_seconds=1)
        p = g.request(request_id="r1", tool_name="delete_note",
                      normalized_args={"id": "all"}, raw_request={},
                      tool_call_id="c1")
        self.assertIs(p, g.expire())
        self.assertIsNone(g.pending)

    def test_accept_executes_exactly_once_with_original_args(self):
        g = ConfirmationGate()
        original = {"id": "all", "x": 1}
        g.request(request_id="r1", tool_name="delete_note",
                  normalized_args=original, raw_request={},
                  tool_call_id="c1")
        p = g.accept()
        self.assertEqual(original, p.normalized_args)   # 原参数, 不重生成
        self.assertIsNone(g.accept())                   # 第二次拿不到 = 不重执行

    def test_restart_invalidates(self):
        """重启后待确认默认失效: 槽位纯内存态, 新实例无 pending。"""
        g = ConfirmationGate()
        g.request(request_id="r1", tool_name="delete_note",
                  normalized_args={"id": "all"}, raw_request={},
                  tool_call_id="c1")
        fresh = ConfirmationGate()
        self.assertIsNone(fresh.pending)
        self.assertEqual("no_pending", fresh.interpret("确认", 0))


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="r5r4_store_")
        self.path = os.path.join(self.dir, "agent_state.sqlite3")
        self.store = AgentStore(self.path)

    def tearDown(self):
        self.store.close()
        import shutil
        shutil.rmtree(self.dir, True)

    @unittest.skipIf(os.name == "nt", "Windows 无 POSIX 权限位; 0600 由 Pi 实机验证")
    def test_permissions_0600(self):
        self.store.open()
        mode = os.stat(self.path).st_mode & 0o777
        self.assertEqual(0o600, mode)

    def test_preference_roundtrip(self):
        self.store.open()
        self.assertEqual("ok", self.store.remember_preference(
            "简洁", "回答尽量简短"))
        rows = self.store.list_preferences()
        self.assertEqual(1, len(rows))
        self.assertEqual("简洁", rows[0]["key"])
        self.assertEqual("not_found", self.store.forget_preference("nope"))
        self.assertEqual("ok", self.store.forget_preference("简洁"))
        self.assertEqual([], self.store.list_preferences())

    def test_preference_all_requires_confirmation(self):
        self.store.open()
        self.store.remember_preference("k", "v")
        self.assertEqual("confirm_required",
                         self.store.forget_preference("all"))
        self.store.forget_all_preferences()
        self.assertEqual([], self.store.list_preferences())

    def test_idempotent_duplicate_returns_previous(self):
        self.store.open()
        args_hash = "abc"
        status, _ = self.store.record_idempotent(
            request_id="r1", tool_call_id="c1", args_hash=args_hash,
            result_code="ok", result_digest="d1")
        self.assertEqual("stored", status)
        status, prev = self.store.record_idempotent(
            request_id="r1", tool_call_id="c1", args_hash=args_hash,
            result_code="DIFFERENT", result_digest="d2")
        self.assertEqual("duplicate", status)
        self.assertEqual("ok", prev["result_code"])
        self.assertEqual("d1", prev["result_digest"])

    def test_summary_bounded(self):
        self.store.open()
        for i in range(105):
            self.store.add_summary("r%d" % i, "答案 %d" % i)
        self.assertLessEqual(self.store.summary_count(), 100)

    def test_corruption_recovery_keeps_backup(self):
        # 直接写坏文件
        with open(self.path, "wb") as f:
            f.write(b"not a sqlite db at all")
        self.store.open()                      # 应自动备份重建
        self.assertEqual("ok", self.store.remember_preference("k", "v"))
        backups = [p for p in os.listdir(self.dir) if "corrupt" in p]
        self.assertTrue(backups, "损坏文件必须备份保留, 不得删除")

    def test_idempotency_pruned_by_ttl(self):
        # TTL 剪枝: 用假时钟把记录变老
        fake_now = [1_000_000.0]

        class FakeStore(AgentStore):
            pass

        st = AgentStore(self.path, clock=lambda: fake_now[0])
        st.open()
        st.record_idempotent(request_id="r1", tool_call_id="c1",
                             args_hash="h", result_code="ok",
                             result_digest="d")
        fake_now[0] += 25 * 3600 * 1000 + 1          # 超过 24h
        st.record_idempotent(request_id="r2", tool_call_id="c2",
                             args_hash="h", result_code="ok",
                             result_digest="d")
        self.assertIsNone(st.lookup_idempotent("r1", "c1"),
                          "超 TTL 的幂等记录必须被清理")
        self.assertIsNotNone(st.lookup_idempotent("r2", "c2"))
        st.close()


class CatalogTests(unittest.TestCase):
    """R5_R4 工作包 B: 统一工具目录一致性。"""

    def test_catalog_covers_all_23(self):
        from holopet_agentd.tools import TOOL_NAMES
        from holopet_agentd.tools.whitelist import (TOOL_CATALOG,
                                                    TOOL_WHITELIST,
                                                    READONLY_TOOLS)
        self.assertEqual(set(TOOL_CATALOG), set(TOOL_WHITELIST) | set(TOOL_NAMES))
        self.assertEqual(len(TOOL_CATALOG), 23)
        for name, meta in TOOL_CATALOG.items():
            self.assertIn(meta["execution_target"], ("python", "cpp"), name)
            self.assertIn(meta["side_effect"],
                          ("none", "reversible", "destructive"), name)
            self.assertIn(meta["confirmation_policy"],
                          ("none", "on_destructive_all"), name)
            self.assertGreater(meta["timeout_ms"], 0, name)

    def test_readonly_tools_match_catalog(self):
        from holopet_agentd.tools.whitelist import (TOOL_CATALOG,
                                                    READONLY_TOOLS)
        expect = {n for n, m in TOOL_CATALOG.items()
                  if m["side_effect"] == "none"}
        self.assertEqual(expect, set(READONLY_TOOLS))

    def test_new_tools_present(self):
        from holopet_agentd.tools.whitelist import TOOL_CATALOG
        for n in ("get_ai_mode", "set_ai_mode", "dismiss_alert",
                  "remember_preference", "list_preferences",
                  "forget_preference"):
            self.assertIn(n, TOOL_CATALOG)

    def test_destructive_all_confirmation_policy(self):
        from holopet_agentd.tools.whitelist import TOOL_CATALOG
        for n in ("cancel_timer", "delete_alarm", "delete_note",
                  "forget_preference"):
            self.assertEqual("on_destructive_all",
                             TOOL_CATALOG[n]["confirmation_policy"], n)
            self.assertEqual("destructive", TOOL_CATALOG[n]["side_effect"], n)

    def test_forget_preference_all_confirms(self):
        from holopet_agentd.tools.whitelist import confirm_required
        self.assertTrue(confirm_required("forget_preference", {"id": "all"}))
        self.assertFalse(confirm_required("forget_preference", {"id": "k1"}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
