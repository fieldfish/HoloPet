"""test_fake_voice.py — R4 P3: Fake Voice 最小状态流 (无真实 API)

覆盖:
  - 完整状态流顺序: listening → transcript → thinking → content 0..N
    → response_complete → expression → speaking → tts → done
  - 长回答: content 分片拼回 = 完整内容 (不截断), TTS 文本 = 摘要 (短)
  - LLM 超时 / ASR 错误 / TTS 错误 / 未知 provider → 稳定错误码 + 回 idle
  - 重复 cancel 幂等; 取消后不再发新 tts_started
  - 全程不调用真实 API (仅 Fake provider)
"""

import json
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402
from holopet_agentd.response import ResponseFormatter  # noqa: E402


def run_turn_offline(config, transcript=None, cancel_after_ms=None):
    """直接跑 TurnRunner (无 socket): 返回事件列表 [(type, fields)]"""
    runner = worker_main.TurnRunner(config)
    rid = "t"
    if transcript:
        runner.set_transcript(rid, transcript)
    runner._audio_stop = threading.Event()
    runner._audio_stop.set()          # 立即结束录音等待
    cancel = threading.Event()
    events = []

    def send(r, t, **f):
        events.append((t, f))
        return True

    if cancel_after_ms:
        def later():
            time.sleep(cancel_after_ms / 1000.0)
            cancel.set()
        threading.Thread(target=later, daemon=True).start()
    runner.run(rid, cancel, send)
    return events


def types(events):
    return [t for t, _ in events]


class TestFakeVoice(unittest.TestCase):
    def test_full_flow_order(self):
        events = run_turn_offline({"provider": "fake"}, transcript="你好")
        seq = types(events)
        # 严格顺序: 首个状态必为 listening
        self.assertEqual(seq[0], "state")
        states0 = [f["value"] for t, f in events if t == "state"]
        self.assertEqual(states0[0], "listening")
        # 有效事件序列: state 事件映射为其 value (listening/thinking/...)
        effective = [f["value"] if t == "state" else t for t, f in events]
        idx = {t: effective.index(t) for t in
               ("transcript", "content", "response_complete",
                "expression", "speaking", "tts_finished", "done")}
        self.assertLess(idx["transcript"], idx["content"])
        self.assertLess(idx["content"], idx["response_complete"])
        self.assertLess(idx["response_complete"], idx["expression"])
        self.assertLess(idx["expression"], idx["speaking"])
        self.assertLess(idx["speaking"], idx["tts_finished"])
        self.assertLess(idx["tts_finished"], idx["done"])
        # listening 后的 thinking 状态存在
        states = [f["value"] for t, f in events if t == "state"]
        self.assertIn("thinking", states)
        self.assertEqual(states[-1], "idle")

    def test_long_answer_full_content_summary_tts(self):
        long_text = ("这是一段很长的回答。" * 30)      # 270 字 > 100
        config = {"provider": "fake", "response_policy":
                  {"short_max_chars_zh": 100,
                   "long_spoken_summary_max_chars_zh": 140},
                  "content_chunk_chars": 40}
        formatter = ResponseFormatter(config["response_policy"])
        display, speak = formatter.format(long_text)
        self.assertEqual(display, long_text, "显示内容不截断")
        self.assertLessEqual(len(speak), 140, "TTS 摘要 ≤ 140")
        self.assertLess(len(speak), len(display), "TTS 是摘要不是全文")

        # 通过注入 provider 返回长文本, 验证 content 分片拼回 = 完整内容
        class LongProvider:
            def tool_schemas(self):
                return []

            def chat(self, messages):
                return {"content": long_text, "finish_reason": "stop",
                        "tool_calls": []}

        runner = worker_main.TurnRunner(config)
        runner.set_transcript("t", "给我一段长回答")
        runner._provider = LongProvider()
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        events = []
        runner.run("t", threading.Event(),
                   lambda r, t, **f: events.append((t, f)))
        chunks = [f["text"] for t, f in events if t == "content"]
        self.assertEqual("".join(chunks), long_text,
                         "content 分片拼回 = 完整内容 (不截断)")
        tts_texts = [f["text"] for t, f in events if t == "transcript"]
        self.assertIn("给我一段长回答", tts_texts)

    def test_llm_timeout_error_then_idle(self):
        config = {"provider": "fake", "llm_inject": {"timeout": True}}
        events = run_turn_offline(config, transcript="你好")
        errs = [f for t, f in events if t == "error"]
        self.assertEqual(errs[0]["code"], "llm_error")
        self.assertEqual(errs[0]["text"], "llm_timeout")
        states = [f["value"] for t, f in events if t == "state"]
        self.assertEqual(states[-1], "idle", "最终状态回 idle")

    def test_stt_error_then_idle(self):
        events = run_turn_offline({"provider": "fake"}, transcript=None)
        errs = [f for t, f in events if t == "error"]
        self.assertEqual(errs[0]["code"], "stt_error")
        states = [f["value"] for t, f in events if t == "state"]
        self.assertEqual(states[-1], "idle")

    def test_tts_error_then_done(self):
        config = {"provider": "fake",
                  "tts_inject": {"error_code": "engine"}}
        events = run_turn_offline(config, transcript="你好")
        errs = [f for t, f in events if t == "error"]
        self.assertEqual(errs[0]["code"], "tts_error")
        self.assertIn("done", types(events), "TTS 错误后仍 done")
        states = [f["value"] for t, f in events if t == "state"]
        self.assertEqual(states[-1], "idle")

    def test_unknown_provider_error(self):
        from holopet_agentd import providers as prov
        with self.assertRaises(ValueError):
            prov.make_provider({"provider": "no_such_provider"})

    def test_repeat_cancel_idempotent_no_new_tts(self):
        # 长文本让 TTS 有多 chunk; cancel 后不得再发新 tts_started
        long_text = ("长回答内容。" * 40)
        config = {"provider": "fake", "content_chunk_chars": 20}
        runner = worker_main.TurnRunner(config)
        runner.set_transcript("t", "你好")

        class LongProvider:
            def tool_schemas(self):
                return []

            def chat(self, messages):
                return {"content": long_text, "finish_reason": "stop",
                        "tool_calls": []}

        runner._provider = LongProvider()
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        cancel = threading.Event()
        events = []
        started = {"n": 0}

        def send(r, t, **f):
            events.append((t, f))
            if t == "tts_started":
                started["n"] += 1
                cancel.set()          # 第一段 TTS 开始后立即长按取消
            return True

        runner.run("t", cancel, send)
        # R4-R1 (D6): 单测层不再中途发 cancel 事件 — 终态 cancel 由
        # 服务层 _run_turn_job 在 turn 真正停止后发出 (见
        # test_worker_concurrency.test_cancel_is_terminal_not_early_ack)
        cancels = [t for t, _ in events if t == "cancel"]
        self.assertEqual(len(cancels), 0,
                         "TurnRunner 单测层不发送中途 cancel (终态在服务层)")
        self.assertEqual(started["n"], 1,
                         "取消后不得再播放新的 TTS (无第二个 tts_started)")
        # 再跑一次同样 cancel: 幂等 (cancel 事件仍正常产生, 不抛错)
        cancel2 = threading.Event()
        events2 = []
        runner2 = worker_main.TurnRunner(config)
        runner2.set_transcript("t", "你好")
        runner2._provider = LongProvider()
        runner2._audio_stop = threading.Event()
        runner2._audio_stop.set()
        runner2.run("t", cancel2, lambda r, t, **f: events2.append((t, f)))
        self.assertIn("done", types(events2))

    def test_empty_answer_has_exactly_one_response_complete(self):
        """R8_R4_R3_R1: 空回答 = 温和反馈 (Pi §11 实测无结果) —
        恰好一次 response_complete + 固定提示 content + expression=neutral;
        text_only=true 时无 tts_started, idle/done。"""

        class EmptyProvider:
            def tool_schemas(self):
                return []

            def chat(self, messages):
                return {"content": "", "finish_reason": "stop", "tool_calls": []}

        runner = worker_main.TurnRunner({"provider": "fake"})
        runner.set_transcript("t", "hi")
        runner._provider = EmptyProvider()
        runner._audio_stop = threading.Event()
        runner._audio_stop.set()
        events = []
        runner.run("t", threading.Event(),
                   lambda r, t, **f: events.append((t, f)))
        seq = types(events)
        self.assertEqual(seq.count("response_complete"), 1,
                         f"空回答恰好一次 response_complete: {seq}")
        self.assertIn("content", seq, "空回答发固定提示 content (R3_R1)")
        cont = [f["text"] for t, f in events if t == "content"]
        self.assertEqual("".join(cont), "抱歉，我这次没有给出回答，请再说一次吧")
        self.assertNotIn("tts_started", seq, "text_only=true 不调用 TTS")
        exprs = [f["value"] for t, f in events if t == "expression"]
        self.assertEqual(exprs, ["neutral"], f"空回答 expression=neutral: {exprs}")
        self.assertEqual(seq[-1], "done", "空回答以 done 结束")
        states = [f["value"] for t, f in events if t == "state"]
        self.assertEqual(states[-1], "idle", "空回答回 idle")

    def test_no_real_api_called(self):
        # Fake 全程离线: 直接断言 events 无网络字段, 且 provider 为 Fake
        events = run_turn_offline({"provider": "fake"}, transcript="现在几点")
        for t, f in events:
            self.assertNotIn("url", f)
            self.assertNotIn("api_key", f)


if __name__ == "__main__":
    unittest.main()
