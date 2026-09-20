"""response/formatter.py — ResponseFormatter (V6-R4 P3)

长短内容策略 (总规范 §12, 字段冻结于 config/response_policy.yaml):
  短回答: display_text = speak_text = 全文
  长回答: display_text = 全文 (经 content 分片发给显示层, 不截断);
          speak_text = 前 1~3 句摘要 (≤ summary_max_chars) 发给 TTS。
"""

import re

_SENTENCE_END = re.compile(r"(?<=[。！？!?])")


class ResponseFormatter:
    def __init__(self, config=None):
        cfg = config or {}
        self.short_max_chars = int(cfg.get("short_max_chars_zh", 100))
        self.summary_max_chars = int(
            cfg.get("long_spoken_summary_max_chars_zh", 140))
        self.max_summary_sentences = int(cfg.get(
            "long_spoken_summary_max_sentences", 3))

    def format(self, text: str):
        """返回 (display_text, speak_text)。display 永不截断。"""
        text = text.strip()
        if len(text) <= self.short_max_chars:
            return text, text
        sentences = [s for s in _SENTENCE_END.split(text) if s.strip()]
        summary = ""
        for s in sentences[:self.max_summary_sentences]:
            if len(summary) + len(s) > self.summary_max_chars:
                break
            summary += s
        if not summary:
            summary = text[:self.summary_max_chars]
        return text, summary.strip()
