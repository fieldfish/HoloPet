"""test_r4r2_config.py — R4-R2 (F4): 配置加载事实澄清测试

断言:
  - load_config 事实仍是 JSON (agent.example.json 可加载, 字段可用)
  - agent.yaml / providers.yaml / response_policy.yaml 是 schema-only
    (存在且不含密钥; 不参与运行时加载)
  - 未知 provider 依旧稳定报 ValueError (不静默回 fake)
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd import providers  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))


class TestR4R2Config(unittest.TestCase):
    def test_json_is_runtime_config(self):
        cfg = worker_main.load_config(os.path.join(
            ROOT, "config", "agent.example.json"))
        self.assertIn("provider", cfg)
        self.assertEqual(cfg["provider"], "fake")

    def test_yaml_files_are_schema_only(self):
        for f in ("agent.yaml", "providers.yaml", "response_policy.yaml"):
            path = os.path.join(ROOT, "config", f)
            self.assertTrue(os.path.isfile(path), f)
            # R4-R2 复验 P1: with open 显式关闭 (消除 ResourceWarning)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            self.assertIn("schema-only", text,
                          f"{f} 必须标注 schema-only (不参与运行时加载)")
            self.assertNotIn("sk-", text)

    def test_unknown_provider_stable_error(self):
        with self.assertRaises(ValueError):
            providers.make_provider({"provider": "no_such_provider"})


if __name__ == "__main__":
    unittest.main()
