"""test_fake_provider.py — Fake Provider (chat 接口) 离线链路测试 (无网络)  V6-R2"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.providers.fake import FakeAgentProvider  # noqa: E402


class TestFakeProvider(unittest.TestCase):
    def setUp(self):
        self.p = FakeAgentProvider({"max_history_turns": 8, "max_tool_rounds": 4})

    def test_chat_greeting(self):
        resp = self.p.chat([{"role": "user", "content": "你好"}])
        self.assertIn("你好", resp["content"])
        self.assertEqual(resp["finish_reason"], "stop")

    def test_chat_time_tool_first_round(self):
        # 第一轮: 模型请求工具; 无 tool 消息
        resp = self.p.chat([{"role": "user", "content": "现在几点"}])
        self.assertEqual(resp["finish_reason"], "tool_calls")
        self.assertEqual(resp["tool_calls"][0]["function"]["name"], "get_time")

    def test_chat_time_tool_second_round_uses_real_result(self):
        # 第二轮: messages 含 tool 消息 (真实 dispatch 结果) → 回答使用该结果
        msgs = [
            {"role": "user", "content": "现在几点"},
            {"role": "assistant", "content": None, "tool_calls": [
                {"id": "call-1", "function": {"name": "get_time", "arguments": "{}"}}]},
            {"role": "tool", "tool_call_id": "call-1", "content": "12:34"},
        ]
        resp = self.p.chat(msgs)
        self.assertIn("12:34", resp["content"])
        self.assertEqual(resp["finish_reason"], "stop")

    def test_chat_emotion(self):
        resp = self.p.chat([{"role": "user", "content": "我有点难过"}])
        self.assertIn("低落", resp["content"])

    def test_no_network_imports(self):
        import holopet_agentd.providers.fake as fake_mod
        self.assertNotIn("urllib", dir(fake_mod))


if __name__ == "__main__":
    unittest.main()
