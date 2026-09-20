"""test_r83_r4_tool_schemas.py — R8_R4_R1 白名单工具 schema 广告合同测试。

背景: Pi B7 实测发现真实链路中 §5 白名单工具从未被广告给任何 LLM
(openai_compat.tool_schemas() 只返回 tools/__init__.py 的 TOOL_REGISTRY,
其中仅 V6 内部工具 4 个)。R1 修复: whitelist.py 提供 TOOL_SCHEMAS,
main.py 导入时把它们扩展进 TOOL_REGISTRY。

本文件断言:
  1. TOOL_SCHEMAS 与 TOOL_WHITELIST 一一对应, 参数集合一致;
  2. schema 为 openai function 格式 (name/description/parameters);
  3. 导入 holopet_agentd.main 后 TOOL_REGISTRY 同时含内部工具与 13 白名单工具,
     且 V6 内部工具保持原样。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.tools.whitelist import (  # noqa: E402
    TOOL_WHITELIST, TOOL_SCHEMAS)


class ToolSchemas(unittest.TestCase):
    def test_schema_count_matches_whitelist(self):
        names = {s["function"]["name"] for s in TOOL_SCHEMAS}
        self.assertEqual(names, set(TOOL_WHITELIST))
        self.assertEqual(len(TOOL_SCHEMAS), len(TOOL_WHITELIST))
        self.assertEqual(len(names), 19)

    def test_schema_openai_format(self):
        for s in TOOL_SCHEMAS:
            self.assertEqual(s.get("type"), "function")
            fn = s["function"]
            self.assertTrue(fn.get("name"))
            self.assertTrue(fn.get("description"))
            params = fn["parameters"]
            self.assertEqual(params.get("type"), "object")
            self.assertIsInstance(params.get("properties"), dict)
            self.assertIsInstance(params.get("required"), list)

    def test_schema_parameter_sets_match_whitelist(self):
        for s in TOOL_SCHEMAS:
            fn = s["function"]
            name = fn["name"]
            required, allowed = TOOL_WHITELIST[name]
            props = set(fn["parameters"]["properties"])
            self.assertEqual(props, required | allowed, name)
            self.assertEqual(set(fn["parameters"]["required"]),
                             required, name)

    def test_schema_property_types_valid(self):
        legal = {"string", "integer", "boolean"}
        for s in TOOL_SCHEMAS:
            for prop in s["function"]["parameters"]["properties"].values():
                self.assertIn(prop.get("type"), legal)

    def test_internal_tools_preserved_after_extension(self):
        import holopet_agentd.main  # noqa: F401 — R1 扩展注册点
        from holopet_agentd.tools import TOOL_REGISTRY
        names = {s["function"]["name"] for s in TOOL_REGISTRY}
        self.assertTrue({"get_time", "get_status", "set_volume",
                         "set_expression"}.issubset(names))

    def test_registry_advertises_whitelist_after_main_import(self):
        import holopet_agentd.main  # noqa: F401 — R1 扩展注册点
        from holopet_agentd.tools import TOOL_REGISTRY
        names = {s["function"]["name"] for s in TOOL_REGISTRY}
        self.assertTrue(set(TOOL_WHITELIST).issubset(names))
        self.assertEqual(len(names), 4 + len(TOOL_WHITELIST))


if __name__ == "__main__":
    unittest.main()
