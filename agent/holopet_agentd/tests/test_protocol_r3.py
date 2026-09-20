"""test_protocol_r3.py — R3 协议硬化测试 (无网络)

覆盖:
  - 所有构造消息含 v + request_id
  - 未知 type → unknown_type (结构化拒绝, 不再静默忽略)
  - 坏 JSON / 非 dict / 超长行 → 稳定错误码
  - read_lines 半包/粘包/EOF 尾部
  - make_error 稳定 code
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import protocol  # noqa: E402


class TestProtocolR3(unittest.TestCase):
    def test_make_message_always_versioned(self):
        m = protocol.make_message("r1", "done")
        self.assertEqual(m["v"], protocol.PROTOCOL_VERSION)
        self.assertEqual(m["request_id"], "r1")

    def test_unknown_type_rejected(self):
        m = protocol.parse_line('{"v":"1","request_id":"r","type":"shell_exec"}')
        self.assertEqual(protocol.validate(m), "unknown_type")

    def test_known_types_accepted(self):
        for t in ("start_turn", "transcript", "stop_recording", "cancel"):
            m = protocol.parse_line(
                f'{{"v":"1","request_id":"r","type":"{t}"}}')
            self.assertIsNone(protocol.validate(m), t)

    def test_missing_request_id_rejected(self):
        m = protocol.parse_line('{"v":"1","type":"start_turn"}')
        self.assertEqual(protocol.validate(m), "bad_request_id")

    def test_bad_json_code(self):
        m = protocol.parse_line("{not json")
        self.assertEqual(m["type"], "__bad__")
        self.assertEqual(protocol.validate(m), "bad_json")

    def test_not_object_code(self):
        m = protocol.parse_line("[1,2,3]")
        self.assertEqual(m["type"], "__bad__")
        self.assertEqual(protocol.validate(m), "not_object")

    def test_line_too_long_code(self):
        m = protocol.parse_line('{"x":"' + "a" * (protocol.MAX_LINE_BYTES + 10) + '"}')
        self.assertEqual(protocol.validate(m), "line_too_long")

    def test_read_lines_partial(self):
        # 半包: 无换行 → 无完整行 (R4: 3-tuple 返回)
        lines, rest, overflow = protocol.read_lines(b"", b'{"type":"done"')
        self.assertEqual(lines, [])
        self.assertEqual(rest, b'{"type":"done"')
        self.assertFalse(overflow)

    def test_read_lines_sticky(self):
        # 粘包: 两条一行到达
        lines, rest, overflow = protocol.read_lines(
            b"", b'{"a":1}\n{"b":2}\n{"c":')
        self.assertEqual(lines, ['{"a":1}', '{"b":2}'])
        self.assertEqual(rest, b'{"c":')
        self.assertFalse(overflow)

    def test_read_lines_chunked_join(self):
        # 分两次到达拼成一行
        lines, rest, overflow = protocol.read_lines(b'{"ty', b'pe":"done"}\n')
        self.assertEqual(lines, ['{"type":"done"}'])
        self.assertEqual(rest, b"")
        self.assertFalse(overflow)

    def test_read_lines_overflow_partial(self):
        # R4: 无换行部分包超过 MAX_LINE_BYTES → overflow, 缓冲清空
        lines, rest, overflow = protocol.read_lines(
            b"", b"a" * (protocol.MAX_LINE_BYTES + 10))
        self.assertEqual(lines, [])
        self.assertEqual(rest, b"")
        self.assertTrue(overflow)

    def test_make_error_stable_code(self):
        e = protocol.make_error("r9", "busy", "busy")
        self.assertEqual(e["code"], "busy")
        self.assertEqual(e["type"], "error")
        self.assertEqual(e["v"], protocol.PROTOCOL_VERSION)
        self.assertEqual(e["request_id"], "r9")


if __name__ == "__main__":
    unittest.main()
