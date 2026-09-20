"""protocol.py — holopet_agentd IPC 协议 (V6-R3)

JSON Lines over Unix Domain Socket (生产/Pi) 或 loopback TCP (Windows 开发回退)。
协议版本与 C++ src/ipc/ai_worker_protocol.hpp 保持一致: "1"

R3 强化 (相对 R2):
  - 所有消息必须含 v + request_id (校验函数强制)
  - 未知 type 不再静默忽略: 结构化拒绝 {"type":"error","code":"unknown_type"}
  - 单行最大长度 MAX_LINE_BYTES; 超限 → 结构化 error (code=line_too_long)
  - 坏行/非 dict → 结构化 error (code=bad_json / not_object)
  - 稳定错误码: bad_json / not_object / line_too_long / unknown_type /
    unknown_request / busy / stt_error / tts_error / tool_rounds_exceeded /
    turn_failed / internal
  - 部分读取/粘包由调用方行缓冲处理 (read_lines 帮助函数)

消息示例:
  {"v":"1","request_id":"turn-1","type":"start_turn"}
  {"v":"1","request_id":"turn-1","type":"state","value":"thinking"}
  {"v":"1","request_id":"turn-1","type":"done"}
"""

import json

PROTOCOL_VERSION = "1"

# C++ 侧认识的消息类型 (与 ai_worker_protocol.hpp kKnown 一致)
KNOWN_TYPES = {
    "start_turn", "state", "transcript", "tts_level",
    "done", "error", "cancel", "expression", "hello",
    "content", "response_complete",   # R4: 完整回答分片与结束标记
    "tool_request", "tool_result", "feature_event",   # R8_R4 工具协议
    "waiting",                           # R8_R4_R2: 慢回答等待提示
}

# 客户端 → agentd 可接受的请求类型 (其余为 agentd → 客户端事件)
REQUEST_TYPES = {"start_turn", "transcript", "stop_recording", "cancel"}

# agentd → 客户端事件 (R4-R1 方向集合; 与 C++ kKnown 一致)
EVENT_TYPES = {
    "hello", "state", "transcript", "content", "response_complete",
    "tts_level", "expression", "tts_started", "tts_finished", "tool_call",
    "done", "error", "cancel",
    "tool_result", "feature_event",   # R8_R4: C++ → agentd 工具回执
    "waiting",                           # R8_R4_R2: agentd → C++ 等待提示
}

# 仅单向的类型: 反向出现 → wrong_direction (transcript/cancel 双向合法)
REQUEST_ONLY = {"start_turn", "stop_recording"}
EVENT_ONLY = {
    "hello", "state", "content", "response_complete", "tts_level",
    "expression", "tts_started", "tts_finished", "tool_call", "done", "error",
}

# 单行最大长度 (字节); 超限按协议错误处理
MAX_LINE_BYTES = 1 * 1024 * 1024


def parse_line(line: str) -> dict:
    """解析一行 JSON。

    合法 → 原 dict (未知字段容忍)。
    非法 → {"type": "__bad__", "code": <稳定错误码>, ...} —
    调用方必须将其转为结构化 error 回包, 不得静默丢弃。
    """
    line = line.strip()
    if not line:
        return {"type": "__bad__", "code": "bad_json", "reason": "empty"}
    if len(line.encode("utf-8", "replace")) > MAX_LINE_BYTES:
        return {"type": "__bad__", "code": "line_too_long",
                "raw_len": len(line)}
    try:
        msg = json.loads(line)
    except (json.JSONDecodeError, ValueError):
        return {"type": "__bad__", "code": "bad_json",
                "reason": "json", "raw_len": len(line)}
    if not isinstance(msg, dict):
        return {"type": "__bad__", "code": "not_object"}
    return msg


def _common_checks(msg: dict):
    """公共结构检查 (R4-R1 D3): 任何畸形对象都不得抛到 reader 线程。

    v 必须为 str 且 == PROTOCOL_VERSION (数值 1 / 缺失 / "2" → bad_version)
    request_id 必须为 str 且 strip 后非空 (数值/对象/列表/空/纯空白 →
    bad_request_id)
    type 必须为 str 且已知 (列表/对象 type → unknown_type, 不抛 TypeError)
    """
    v = msg.get("v")
    if not isinstance(v, str) or v != PROTOCOL_VERSION:
        return "bad_version"
    rid = msg.get("request_id")
    if not isinstance(rid, str) or not rid.strip():
        return "bad_request_id"
    mtype = msg.get("type")
    if not isinstance(mtype, str):
        return "unknown_type"
    if mtype not in KNOWN_TYPES and mtype not in REQUEST_TYPES \
            and mtype not in EVENT_TYPES:
        return "unknown_type"
    return None


def validate_request(msg: dict):
    """校验 agentd 收到的客户端请求 (R4-R1 D3: 方向合同)。

    允许: REQUEST_TYPES; 事件专属类型 (hello/content 等) → wrong_direction。
    """
    if msg.get("type") == "__bad__":
        return msg.get("code", "bad_json")
    mtype = msg.get("type")
    if not isinstance(mtype, str):
        return "unknown_type"
    if mtype in EVENT_ONLY:
        return "wrong_direction"
    return _common_checks(msg)


def validate_event(msg: dict):
    """校验 C++ 收到的 agentd 事件 (方向合同)。

    hello 为握手例外 (无 v/request_id); 请求专属类型 (start_turn/
    stop_recording) → wrong_direction。
    """
    if msg.get("type") == "__bad__":
        return msg.get("code", "bad_json")
    mtype = msg.get("type")
    if not isinstance(mtype, str):
        return "unknown_type"
    if mtype in REQUEST_ONLY:
        return "wrong_direction"
    if mtype == "hello":
        return None
    return _common_checks(msg)


def validate(msg: dict):
    """兼容入口: 无方向上下文的公共结构检查 (测试/工具用)。"""
    if msg.get("type") == "__bad__":
        return msg.get("code", "bad_json")
    if msg.get("type") == "hello":
        return None
    return _common_checks(msg)


def encode(msg: dict) -> str:
    """序列化一行 (追加 \\n)"""
    return json.dumps(msg, ensure_ascii=False) + "\n"


def make_message(request_id: str, msg_type: str, **fields) -> dict:
    """构造带协议版本的消息 (未知字段 = 未来扩展)"""
    msg = {"v": PROTOCOL_VERSION, "request_id": request_id, "type": msg_type}
    msg.update(fields)
    return msg


def make_error(request_id: str, code: str, message: str = "") -> dict:
    """稳定错误码结构 (R3): code 供程序判定, text 供人读。"""
    msg = make_message(request_id, "error", code=code, text=message or code)
    return msg


def is_known_type(msg_type: str) -> bool:
    return msg_type in KNOWN_TYPES or msg_type in REQUEST_TYPES


def read_lines(buf: bytes, data: bytes):
    """行缓冲: 处理半包/粘包 (R4: 半包超限防护)。

    返回 (lines, rest, overflow):
      lines    — 完整行 (str) 列表
      rest     — 不完整尾部 (无换行); overflow 时被清空以释放内存
      overflow — True 表示无换行的部分包已超过 MAX_LINE_BYTES
                 (调用方必须回 line_too_long 并重置缓冲, 不得无限增长)
    EOF 判定由调用方负责 (recv 空 → rest 非空即半包截断)。
    """
    buf = buf + data
    lines = []
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        try:
            lines.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            # R4-R1 (D4): 非法 UTF-8 两侧同策略 — 送入 JSON parser
            # 后稳定报 bad_json (不 replacement / 不原字节)
            lines.append("{invalid-utf8}")
    if len(buf) > MAX_LINE_BYTES:
        return lines, b"", True
    return lines, buf, False
