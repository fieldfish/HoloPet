"""test_response_interpreter.py — R8_R2 工作包 C: 模型输出解释层。

覆盖: 合法 JSON (text+emotion) / 纯文本 / 未知情绪回落 / 缺 text /
空 text / 畸形 JSON / 中文+emoji / 非对象 JSON / text 非字符串 /
text 首尾空白裁剪。
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.response.interpreter import (  # noqa: E402
    EMOTION_WHITELIST, interpret,
)


class TestInterpreter(unittest.TestCase):
    def test_valid_json_text_and_emotion(self):
        r = interpret('{"text":"你好，我是 HoloPet。","emotion":"happy"}')
        self.assertEqual(r.text, "你好，我是 HoloPet。")
        self.assertEqual(r.emotion, "happy")
        self.assertTrue(r.was_json_object)

    def test_json_text_trimmed(self):
        r = interpret('{"text":"  带空白  ","emotion":"neutral"}')
        self.assertEqual(r.text, "带空白")

    def test_unknown_emotion_falls_back(self):
        r = interpret('{"text":"ok","emotion":"furious"}')
        self.assertEqual(r.emotion, "neutral")
        self.assertEqual(r.text, "ok")

    def test_emotion_case_and_space_normalized(self):
        r = interpret('{"text":"ok","emotion":" HAPPY "}')
        self.assertEqual(r.emotion, "happy")

    def test_missing_text_falls_back_to_raw(self):
        raw = '{"emotion":"happy"}'
        r = interpret(raw)
        self.assertEqual(r.text, raw)          # 不丢字: 整段原文
        self.assertEqual(r.emotion, "happy")

    def test_empty_text_falls_back_to_raw(self):
        raw = '{"text":"   ","emotion":"sad"}'
        r = interpret(raw)
        self.assertEqual(r.text, raw)
        self.assertEqual(r.emotion, "sad")

    def test_text_wrong_type_falls_back(self):
        raw = '{"text":123,"emotion":"happy"}'
        r = interpret(raw)
        self.assertEqual(r.text, raw)
        self.assertEqual(r.emotion, "happy")

    def test_malformed_json_is_plain_text(self):
        raw = '{"text":"未闭合'
        r = interpret(raw)
        self.assertEqual(r.text, raw)
        self.assertEqual(r.emotion, "neutral")
        self.assertFalse(r.was_json_object)

    def test_non_object_json_is_plain_text(self):
        raw = "[1,2,3]"
        r = interpret(raw)
        self.assertEqual(r.text, raw)
        self.assertEqual(r.emotion, "neutral")

    def test_plain_text(self):
        r = interpret("  纯文本回答，没有 JSON。 ")
        self.assertEqual(r.text, "纯文本回答，没有 JSON。")
        self.assertEqual(r.emotion, "neutral")

    def test_chinese_and_emoji_preserved(self):
        text = "今天天气不错 😊🌧️，记得带伞。"
        r = interpret(json.dumps({"text": text, "emotion": "curious"},
                                 ensure_ascii=False))
        self.assertEqual(r.text, text)
        self.assertEqual(r.emotion, "curious")

    def test_all_whitelist_emotions(self):
        for e in EMOTION_WHITELIST:
            r = interpret(json.dumps({"text": "t", "emotion": e}))
            self.assertEqual(r.emotion, e)

    def test_empty_input(self):
        r = interpret("")
        self.assertEqual(r.text, "")
        self.assertEqual(r.emotion, "neutral")

    def test_non_str_input_defensive(self):
        r = interpret(None)          # 防御: 不抛异常
        self.assertEqual(r.text, "")
        self.assertEqual(r.emotion, "neutral")


if __name__ == "__main__":
    unittest.main()
