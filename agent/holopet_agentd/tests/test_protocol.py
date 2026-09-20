"""test_protocol.py — 协议层单元测试 (无网络)"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import protocol  # noqa: E402


class TestProtocol(unittest.TestCase):
    def test_version(self):
        self.assertEqual(protocol.PROTOCOL_VERSION, "1")

    def test_parse_valid(self):
        m = protocol.parse_line('{"request_id":"r","type":"transcript","text":"你好"}')
        self.assertEqual(m["request_id"], "r")
        self.assertEqual(m["text"], "你好")

    def test_parse_unknown_fields_tolerated(self):
        m = protocol.parse_line('{"request_id":"r","type":"done","future":123}')
        self.assertEqual(m["type"], "done")

    def test_parse_bad_json(self):
        m = protocol.parse_line("{not json")
        self.assertEqual(m["type"], "__bad__")

    def test_parse_empty(self):
        m = protocol.parse_line("")
        self.assertEqual(m["type"], "__bad__")

    def test_parse_not_dict(self):
        m = protocol.parse_line("[1,2,3]")
        self.assertEqual(m["type"], "__bad__")

    def test_encode_roundtrip(self):
        msg = protocol.make_message("r1", "state", value="thinking")
        line = protocol.encode(msg)
        back = protocol.parse_line(line)
        self.assertEqual(back["v"], "1")
        self.assertEqual(back["type"], "state")
        self.assertEqual(back["value"], "thinking")

    def test_known_types(self):
        for t in ("start_turn", "state", "transcript", "tts_level",
                  "done", "error", "cancel", "expression", "hello"):
            self.assertTrue(protocol.is_known_type(t), t)
        self.assertFalse(protocol.is_known_type("mystery"))

    def test_encode_unicode(self):
        line = protocol.encode(protocol.make_message("r", "transcript", text="你好世界"))
        self.assertIn("你好世界", line)
        self.assertTrue(line.endswith("\n"))


if __name__ == "__main__":
    unittest.main()
