"""test_prompt_loader.py — R8_R2 工作包 A: 分文件 Prompt 组合加载器。

覆盖: 固定顺序 / 缺目录 / 缺必需文件 / 空文件与空组合 / 非 UTF-8 /
超限 / 组合内容 = 各文件按序拼接。
"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.prompt_loader import (  # noqa: E402
    PromptComposer, PromptError, REQUIRED_PROMPT_FILES,
)


def _write_dir(root, files):
    os.makedirs(root, exist_ok=True)
    for name, body in files.items():
        mode = "wb" if isinstance(body, bytes) else "w"
        kwargs = {} if isinstance(body, bytes) else {"encoding": "utf-8",
                                                     "newline": "\n"}
        with open(os.path.join(root, name), mode, **kwargs) as f:
            f.write(body)


def _full_files():
    return {name: f"part-{i}\n" for i, name in enumerate(REQUIRED_PROMPT_FILES)}


class TestPromptComposer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="r8r2_prompt_")
        self.dir = os.path.join(self.tmp, "prompts")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fixed_order_join(self):
        files = _full_files()
        _write_dir(self.dir, files)
        text = PromptComposer(prompt_dir=self.dir).compose()
        for name in REQUIRED_PROMPT_FILES:
            self.assertIn(files[name].strip(), text)
        # 顺序: 每个文件片段的位置递增
        pos = [text.index(files[n].strip()) for n in REQUIRED_PROMPT_FILES]
        self.assertEqual(pos, sorted(pos), "组合顺序必须按文件名升序")

    def test_missing_dir(self):
        with self.assertRaises(PromptError) as cm:
            PromptComposer(prompt_dir=os.path.join(self.tmp, "nope")).compose()
        self.assertEqual(cm.exception.code, "prompt_dir_missing")

    def test_missing_required_file(self):
        files = _full_files()
        del files["30_tools.md"]
        _write_dir(self.dir, files)
        with self.assertRaises(PromptError) as cm:
            PromptComposer(prompt_dir=self.dir).compose()
        self.assertEqual(cm.exception.code, "prompt_file_missing")
        self.assertIn("30_tools.md", cm.exception.detail)

    def test_empty_files_compose_error(self):
        _write_dir(self.dir, {n: "" for n in REQUIRED_PROMPT_FILES})
        with self.assertRaises(PromptError) as cm:
            PromptComposer(prompt_dir=self.dir).compose()
        self.assertEqual(cm.exception.code, "prompt_empty")

    def test_single_empty_file_ok_if_others_nonempty(self):
        files = _full_files()
        files["60_safety.md"] = ""
        _write_dir(self.dir, files)
        text = PromptComposer(prompt_dir=self.dir).compose()
        self.assertIn("part-0", text)

    def test_non_utf8_rejected(self):
        files = _full_files()
        files["20_behavior.md"] = b"\xff\xfe\x00broken"
        _write_dir(self.dir, files)
        with self.assertRaises(PromptError) as cm:
            PromptComposer(prompt_dir=self.dir).compose()
        self.assertEqual(cm.exception.code, "prompt_not_utf8")

    def test_bom_tolerated(self):
        files = _full_files()
        files["00_identity.md"] = "\ufeffidentity-with-bom\n"
        _write_dir(self.dir, files)
        text = PromptComposer(prompt_dir=self.dir).compose()
        self.assertNotIn("\ufeff", text)

    def test_too_large_rejected(self):
        files = _full_files()
        files["10_personality.md"] = "x" * 200
        _write_dir(self.dir, files)
        with self.assertRaises(PromptError) as cm:
            PromptComposer(prompt_dir=self.dir, max_bytes=100).compose()
        self.assertEqual(cm.exception.code, "prompt_too_large")

    def test_default_dir_is_package_prompts(self):
        # 真实包内 prompts/ 必须可组合 (7 文件全在)
        text = PromptComposer().compose()
        self.assertGreater(len(text.encode("utf-8")), 500)
        self.assertIn("HoloPet", text)

    def test_from_config_relative_dir(self):
        c = PromptComposer.from_config({"prompt_dir": "prompts",
                                        "prompt_max_bytes": 65536})
        self.assertTrue(c.prompt_dir.endswith("prompts"))
        self.assertEqual(c.max_bytes, 65536)


if __name__ == "__main__":
    unittest.main()
