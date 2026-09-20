"""response/interpreter.py — 模型输出解释层 (R8_R2 工作包 C)

把模型返回的 content 文本解释为 (display_text, emotion):

  1. 合法 JSON 对象: 读取 text (必须 str, 去首尾空白) 与 emotion;
     emotion 不在白名单时回落 neutral; text 缺失/非 str/为空 →
     回退整段原文为显示文本 (emotion=neutral)。
  2. 非 JSON 纯文本: 全文显示, emotion=neutral。
  3. 畸形 JSON (以 { 开头但解析失败): 视作纯文本全文显示, 不崩溃。
  4. 合法 JSON 但非对象 (list/number/string): 视作纯文本全文显示。

保证: 绝不因解析失败抛异常; display_text 永不丢字 (回退=原文)。

白名单与 C++ Emotion 枚举一致:
  neutral happy curious surprised sad sleepy concerned
"""

import json

EMOTION_WHITELIST = ("neutral", "happy", "curious", "surprised",
                     "sad", "sleepy", "concerned",
                     # R8_R4_R3_R4_R5: 九种正式参考表情追加
                     "craving", "speechless", "joy", "angry", "cute")


class Interpretation:
    __slots__ = ("text", "emotion", "was_json_object")

    def __init__(self, text: str, emotion: str, was_json_object: bool):
        self.text = text
        self.emotion = emotion
        self.was_json_object = was_json_object


def _norm_emotion(value) -> str:
    if isinstance(value, str):
        v = value.strip().lower()
        if v in EMOTION_WHITELIST:
            return v
    return "neutral"


def interpret(content: str) -> Interpretation:
    """返回 Interpretation; 任何输入都不抛异常。"""
    raw = content if isinstance(content, str) else ""
    stripped = raw.strip()
    if stripped.startswith("{"):
        try:
            obj = json.loads(stripped)
        except (json.JSONDecodeError, ValueError):
            # 畸形 JSON: 按纯文本回退 (整段原文)
            return Interpretation(raw.strip(), "neutral", False)
        if isinstance(obj, dict):
            text = obj.get("text")
            emotion = _norm_emotion(obj.get("emotion"))
            if isinstance(text, str) and text.strip():
                return Interpretation(text.strip(), emotion, True)
            # JSON 对象但无有效 text: 回退整段原文 (不丢字)
            return Interpretation(raw.strip(), emotion, True)
        # 非对象 JSON: 纯文本回退
        return Interpretation(raw.strip(), "neutral", False)
    return Interpretation(raw.strip(), "neutral", False)
