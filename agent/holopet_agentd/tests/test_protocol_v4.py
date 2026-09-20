"""test_protocol_v4.py — R4-R1 P2: 跨语言 IPC v1 严格合同 (Python 侧)

与 C++ (tests/test_ipc_contract.cpp) 读取同一份
tests/fixtures/ipc_v1/cases.json。R4-R1 变化:
  - 方向合同: line case 分别经 validate_request (agentd 收到的请求)
    与 validate_event (C++ 收到的事件) 断言; C++ 侧是事件接收方
  - stream case 覆盖: CRLF / 完整超长行加换行 / 合法行+超长尾包 /
    恰好 MAX_LINE_BYTES 与 MAX+1 / EOF 残包
  - case 数量由 fixture 动态读取 (不手写)
"""

import base64
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import protocol  # noqa: E402

FIXTURE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))),
    "tests", "fixtures", "ipc_v1", "cases.json")


def _expected(case, role):
    exp = case["expect"]
    if isinstance(exp, dict):
        return exp[role]
    return exp


def run_line_case(case, role):
    """role: "request" (validate_request) | "event" (validate_event)"""
    raw_b = base64.b64decode(case["input_b64"])
    try:
        raw = raw_b.decode("utf-8")
    except UnicodeDecodeError:
        # F3: 非法 UTF-8 与 read_lines 同策略 → {invalid-utf8} → bad_json
        raw = "{invalid-utf8}"
    msg = protocol.parse_line(raw)
    if role == "request":
        code = protocol.validate_request(msg)
    else:
        code = protocol.validate_event(msg)
    return True if code is None else code


def run_stream_case(case):
    """返回 (actual_lines, actual_overflow); lines: [True | 错误码]"""
    buf = b""
    overflow = False
    synth = case.get("synthetic")
    if synth == "overlong_partial":
        lines, buf, overflow = protocol.read_lines(
            b"", b"a" * (protocol.MAX_LINE_BYTES + 10))
    elif synth == "legal_then_overlong_tail":
        legal = ('{"v":"1","request_id":"a","type":"done"}\n').encode()
        big = b"b" * (protocol.MAX_LINE_BYTES + 10)
        lines, buf, overflow = protocol.read_lines(b"", legal + big)
    elif synth == "overlong_line_with_newline":
        big = b"c" * (protocol.MAX_LINE_BYTES + 10) + b"\n"
        lines, buf, overflow = protocol.read_lines(b"", big)
    elif synth == "exact_max_partial":
        lines, buf, overflow = protocol.read_lines(
            b"", b"d" * protocol.MAX_LINE_BYTES)
    elif synth == "max_plus_one_partial":
        lines, buf, overflow = protocol.read_lines(
            b"", b"e" * (protocol.MAX_LINE_BYTES + 1))
    else:
        raw = base64.b64decode(case["input_b64"])
        lines, buf, overflow = protocol.read_lines(b"", raw)
        if case.get("chunk2_b64"):
            raw2 = base64.b64decode(case["chunk2_b64"])
            more, buf, ov2 = protocol.read_lines(buf, raw2)
            lines += more
            overflow = overflow or ov2
    out = []
    for ln in lines:
        code = protocol.validate_event(protocol.parse_line(ln))
        out.append(True if code is None else code)
    return out, overflow


class TestProtocolV4(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(FIXTURE, encoding="utf-8") as f:
            cls.doc = json.load(f)
        cls.cases = cls.doc["cases"]
        cls.n_line = sum(1 for c in cls.cases if c["kind"] == "line")
        cls.n_stream = sum(1 for c in cls.cases if c["kind"] == "stream")

    def test_fixture_version(self):
        self.assertEqual(self.doc["protocol_version"], "1")

    def test_dynamic_case_counts(self):
        # 数量由 fixture 动态读取 (报告不得手写 case 数)
        self.assertGreaterEqual(self.n_line, 26)
        self.assertGreaterEqual(self.n_stream, 10)

    def test_all_line_cases_request_role(self):
        for case in self.cases:
            if case["kind"] != "line":
                continue
            with self.subTest(role="request", case=case["name"]):
                self.assertEqual(run_line_case(case, "request"),
                                 _expected(case, "request"), case["name"])

    def test_all_line_cases_event_role(self):
        for case in self.cases:
            if case["kind"] != "line":
                continue
            with self.subTest(role="event", case=case["name"]):
                self.assertEqual(run_line_case(case, "event"),
                                 _expected(case, "event"), case["name"])

    def test_all_stream_cases(self):
        for case in self.cases:
            if case["kind"] != "stream":
                continue
            with self.subTest(case=case["name"]):
                actual_lines, actual_overflow = run_stream_case(case)
                self.assertEqual(actual_overflow,
                                 case.get("overflow", False),
                                 f"{case['name']} overflow")
                self.assertEqual(actual_lines, case["expect"],
                                 f"{case['name']} lines")

    def test_no_typeerror_on_malformed(self):
        # D3: 畸形 type/v/request_id 不得抛 TypeError 到 reader 线程
        for bad in ('{"v":"1","request_id":"r","type":[]}',
                    '{"v":"1","request_id":"r","type":{"a":1}}',
                    '{"v":1,"request_id":"r","type":"cancel"}',
                    '{"v":"1","request_id":123,"type":"cancel"}'):
            msg = protocol.parse_line(bad)
            code = protocol.validate_request(msg)
            self.assertIn(code,
                          ("bad_version", "bad_request_id", "unknown_type"))


if __name__ == "__main__":
    unittest.main()
