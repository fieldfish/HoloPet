"""test_tools.py — 工具白名单/危险名/分发测试 (无网络)"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import tools  # noqa: E402


class TestTools(unittest.TestCase):
    def test_whitelist(self):
        for name in ("get_time", "get_status", "set_volume", "set_expression"):
            self.assertTrue(tools.is_allowed(name), name)

    def test_unknown_rejected(self):
        self.assertFalse(tools.is_allowed("run_shell"))
        self.assertFalse(tools.is_allowed(""))
        with self.assertRaises(ValueError):
            tools.dispatch("run_shell", {})

    def test_dangerous_patterns(self):
        for name in ("shell", "exec_x", "subprocess.run", "os.system",
                     "import os", "__builtins__"):
            self.assertTrue(tools.is_dangerous_name(name), name)
        self.assertFalse(tools.is_dangerous_name("get_time"))

    def test_whitelist_self_consistent(self):
        for name in tools.TOOL_NAMES:
            self.assertFalse(tools.is_dangerous_name(name), name)

    def test_dispatch_get_time(self):
        result = tools.dispatch("get_time", {})
        self.assertIn("time", result)

    def test_dispatch_set_volume_clamped(self):
        self.assertEqual(tools.dispatch("set_volume", {"volume": 5.0})["volume"], 1.0)
        self.assertEqual(tools.dispatch("set_volume", {"volume": -1})["volume"], 0.0)

    def test_dispatch_set_expression(self):
        self.assertEqual(
            tools.dispatch("set_expression", {"emotion": "happy"})["emotion"], "happy")

    def test_schemas_exposed(self):
        # R8_R4_R1: main.py 导入时会扩展 TOOL_REGISTRY (白名单工具 schema),
        # 故只断言内部工具与格式合同, 不断言总数。
        names = {s["function"]["name"] for s in tools.TOOL_REGISTRY}
        self.assertTrue(set(tools.TOOL_NAMES).issubset(names))
        for schema in tools.TOOL_REGISTRY:
            self.assertEqual(schema["type"], "function")
            self.assertIn("parameters", schema["function"])


if __name__ == "__main__":
    unittest.main()
