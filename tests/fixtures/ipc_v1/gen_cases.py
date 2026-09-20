#!/usr/bin/env python3
"""gen_cases.py — 重新生成 cases.json (R4-R1 P2 严格合同 fixture)

schema:
  line case:   {name, kind="line", input_b64, expect}
  stream case: {name, kind="stream", input_b64|synthetic, [chunk2_b64],
                expect=[...], overflow=bool}

expect 两种形态:
  - 标量 (true | 错误码): 双向类型, 请求/事件两个角色同期望
  - {"request": X, "event": Y}: 按方向区分 (start_turn/hello/content 等
    单向类型) — C++ 侧是事件接收方取 "event"; Python 侧
    validate_request 取 "request"、validate_event 取 "event"。
"""

import base64
import json
import os


def b64(s: str) -> str:
    return base64.b64encode(s.encode("utf-8")).decode("ascii")


def bidir(v):
    return v              # 双向类型: 标量期望


def dirs(req, ev):
    return {"request": req, "event": ev}


cases = [
    # ---- 方向: 请求专属 / 事件专属 ----
    {"name": "ok_start_turn", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"start_turn"}'),
     "expect": dirs(True, "wrong_direction")},
    {"name": "ok_stop_recording", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"stop_recording"}'),
     "expect": dirs(True, "wrong_direction")},
    {"name": "ok_hello_no_v", "kind": "line",
     "input_b64": b64('{"type":"hello"}'),
     "expect": dirs("wrong_direction", True)},
    {"name": "ok_content_chinese_escape", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"content","text":"你好，\\"世界\\"\\n第二行\\t制表"}'),
     "expect": dirs("wrong_direction", True)},
    {"name": "ok_response_complete", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"response_complete"}'),
     "expect": dirs("wrong_direction", True)},
    {"name": "ok_old_request_id_parses", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"stale-rid","type":"state","value":"listening"}'),
     "expect": dirs("wrong_direction", True)},
    # ---- 双向类型 (transcript/cancel) ----
    {"name": "ok_transcript_both_directions", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"transcript","text":"你好"}'),
     "expect": bidir(True)},
    {"name": "ok_cancel_both_directions", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"cancel"}'),
     "expect": bidir(True)},
    # ---- v / request_id / type 畸形 (D3 审计探针) ----
    {"name": "bad_missing_v", "kind": "line",
     "input_b64": b64('{"request_id":"r1","type":"start_turn"}'),
     "expect": dirs("bad_version", "wrong_direction")},
    {"name": "bad_v2", "kind": "line",
     "input_b64": b64('{"v":"2","request_id":"r1","type":"start_turn"}'),
     "expect": dirs("bad_version", "wrong_direction")},
    {"name": "bad_numeric_v", "kind": "line",
     "input_b64": b64('{"v":1,"request_id":"r1","type":"start_turn"}'),
     "expect": dirs("bad_version", "wrong_direction")},
    {"name": "bad_empty_request_id", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"","type":"start_turn"}'),
     "expect": dirs("bad_request_id", "wrong_direction")},
    {"name": "bad_spaces_request_id", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"   ","type":"start_turn"}'),
     "expect": dirs("bad_request_id", "wrong_direction")},
    {"name": "bad_numeric_request_id", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":123,"type":"start_turn"}'),
     "expect": dirs("bad_request_id", "wrong_direction")},
    {"name": "bad_list_type", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":["state"]}'),
     "expect": bidir("unknown_type")},
    {"name": "bad_object_type", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":{"a":1}}'),
     "expect": bidir("unknown_type")},
    {"name": "bad_unknown_type", "kind": "line",
     "input_b64": b64('{"v":"1","request_id":"r1","type":"shell_exec"}'),
     "expect": bidir("unknown_type")},
    {"name": "bad_json", "kind": "line",
     "input_b64": b64("{not json"),
     "expect": bidir("bad_json")},
    {"name": "bad_not_object", "kind": "line",
     "input_b64": b64("[1,2,3]"),
     "expect": bidir("not_object")},
    # ---- F3: 非法 UTF-8 码点 (原始字节, 两侧必须同一 bad_json) ----
    {"name": "bad_utf8_invalid_continuation", "kind": "line",
     "input_b64": base64.b64encode(bytes([0x80])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "bad_utf8_truncated_multibyte", "kind": "line",
     "input_b64": base64.b64encode(bytes([0xE4, 0xBD])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "bad_utf8_overlong_c0_af", "kind": "line",
     "input_b64": base64.b64encode(bytes([0xC0, 0xAF])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "bad_utf8_surrogate_ed_a0_80", "kind": "line",
     "input_b64": base64.b64encode(bytes([0xED, 0xA0, 0x80])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "bad_utf8_beyond_f4_90_80_80", "kind": "line",
     "input_b64": base64.b64encode(bytes([0xF4, 0x90, 0x80, 0x80])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "bad_utf8_overlong_e0_80_80", "kind": "line",
     "input_b64": base64.b64encode(bytes([0xE0, 0x80, 0x80])).decode("ascii"),
     "expect": bidir("bad_json")},
    {"name": "ok_utf8_valid_4byte", "kind": "line",
     "input_b64": base64.b64encode(
         ('{"v":"1","request_id":"r","type":"content","text":"'
          + chr(0x1F600) + '"}').encode("utf-8")).decode("ascii"),
     "expect": dirs("wrong_direction", True)},
    # ---- stream cases (全部双向/中性类型, 两角色同期望) ----
    {"name": "sticky_two_lines", "kind": "stream",
     "input_b64": b64('{"v":"1","request_id":"a","type":"state","value":"thinking"}\n'
                      '{"v":"1","request_id":"a","type":"done"}\n'),
     "expect": [True, True], "overflow": False},
    {"name": "crlf_line_endings", "kind": "stream",
     "input_b64": b64('{"v":"1","request_id":"a","type":"done"}\r\n'),
     "expect": [True], "overflow": False},
    {"name": "partial_then_complete", "kind": "stream",
     "input_b64": b64('{"v":"1","requ'),
     "chunk2_b64": b64('est_id":"a","type":"done"}\n'),
     "expect": [True], "overflow": False},
    {"name": "bad_utf8_line_in_stream", "kind": "stream",
     "input_b64": base64.b64encode(b"\xff\xfe\n").decode("ascii"),
     "expect": ["bad_json"], "overflow": False},
    {"name": "partial_eof_no_line", "kind": "stream",
     "input_b64": b64('{"v":"1","requ'),
     "expect": [], "overflow": False},
    {"name": "overlong_partial_no_newline", "kind": "stream",
     "synthetic": "overlong_partial",
     "expect": [], "overflow": True},
    {"name": "legal_line_plus_overlong_tail", "kind": "stream",
     "synthetic": "legal_then_overlong_tail",
     "expect": [True], "overflow": True},
    {"name": "complete_overlong_line_with_newline", "kind": "stream",
     "synthetic": "overlong_line_with_newline",
     "expect": ["line_too_long"], "overflow": False},
    {"name": "exact_max_partial", "kind": "stream",
     "synthetic": "exact_max_partial",
     "expect": [], "overflow": False},
    {"name": "max_plus_one_partial", "kind": "stream",
     "synthetic": "max_plus_one_partial",
     "expect": [], "overflow": True},
]

doc = {"protocol_version": "1", "cases": cases}
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases.json")
with open(path, "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
print("written", path, "cases:", len(cases))
