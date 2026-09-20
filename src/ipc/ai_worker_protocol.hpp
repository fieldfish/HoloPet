#pragma once
/**
 * ai_worker_protocol.hpp — AI Worker IPC 协议 (V6-R2)
 *
 * JSON Lines over TCP (只监听/连接 127.0.0.1); 协议版本 "1"。
 * R2: 基于 mini_json.hpp 完整解析 (结构化 protocol_error, 不再用脆弱字段提取);
 *     转义/Unicode/截断/错误类型全部覆盖。
 *
 * 消息示例:
 *   {"v":"1","request_id":"uuid","type":"start_turn"}
 *   {"v":"1","request_id":"uuid","type":"state","value":"thinking"}
 *   {"v":"1","request_id":"uuid","type":"transcript","text":"你好"}
 *   {"v":"1","request_id":"uuid","type":"tts_level","rms":0.42}
 *   {"v":"1","request_id":"uuid","type":"done"}
 */

#include "system/mini_json.hpp"

#include <optional>
#include <string>

namespace holopet {

/** 协议版本 (C++ 与 Python worker 必须一致) */
inline constexpr const char* kWorkerProtocolVersion = "1";

/** R4-R1 (D4): 单行最大长度 (UTF-8 原始字节), 与 Python MAX_LINE_BYTES 一致 */
inline constexpr size_t kMaxWorkerLineBytes = 1 * 1024 * 1024;

/** R4-R2 (F3): 非法 UTF-8 两侧同策略 → bad_json。
 * 按 RFC 3629 收紧: 拒绝 C0/C1 过长编码、E0 过长区、ED 代理区 (U+D800..DFFF)、
 * F0 过长区、F4 超范围 (>U+10FFFF)。Python 字符串路径不会产生这些码点,
 * 恶意原始字节在两侧必须得到同一 bad_json。 */
inline bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if ((c & 0xE0) == 0xC0) {            // U+0080..U+07FF
            if (c < 0xC2) return false;      // C0/C1: 过长编码 (2 字节能表达 ASCII)
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {     // U+0800..U+FFFF
            need = 2;
            if (c == 0xE0) lo = 0xA0;        // 过长: < U+0800 的三字节编码
            else if (c == 0xED) hi = 0x9F;   // 代理区 U+D800..U+DFFF
        } else if ((c & 0xF8) == 0xF0) {     // U+10000..U+10FFFF
            need = 3;
            if (c == 0xF0) lo = 0x90;        // 过长: < U+10000 的四字节编码
            else if (c == 0xF4) hi = 0x8F;   // 超范围: > U+10FFFF
        } else {
            return false;
        }
        if (i + need >= n) return false;     // 截断
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            // F3 修正: lo/hi 边界约束只作用于首个续字节 (E0/ED/F0/F4);
            // 其余续字节一律 0x80..0xBF (否则 0x80 这类合法尾字节被误拒)
            const unsigned char c_lo = (k == 1) ? lo : 0x80;
            const unsigned char c_hi = (k == 1) ? hi : 0xBF;
            if (cc < c_lo || cc > c_hi) return false;
        }
        i += need + 1;
    }
    return true;
}

/** 解析出的一条 worker 消息 */
struct WorkerMessage {
    std::string request_id;
    std::string type;          // 见 kKnownTypes (R4: + content / response_complete)
    std::string value;         // state 值 / expression 值
    std::string text;          // transcript/content/error 说明
    std::string code;          // R8_R2_R6: 稳定公开错误码 (llm_*), error 专用
    double      rms = 0.0;     // tts_level
    bool        known_type = false;
};

/** 解析结果: 有效消息 或 结构化协议错误 (二选一) */
struct WorkerParseResult {
    std::optional<WorkerMessage> message;
    std::optional<ProtocolError>  error;
    static WorkerParseResult ok(WorkerMessage m) {
        WorkerParseResult r; r.message = std::move(m); return r;
    }
    static WorkerParseResult fail(std::string reason, size_t pos = 0) {
        WorkerParseResult r;
        r.error = ProtocolError{std::move(reason), pos};
        return r;
    }
};

/** JSON 字符串转义 (便捷, 内部走 mini_json 序列化) */
inline std::string jsonEscape(const std::string& in) {
    JsonValue v; v.type = JsonValue::Type::String; v.str = in;
    return serializeJson(v);
}

/** 构造 outgoing 消息行 (JSON Lines) */
inline std::string buildOutMessage(const std::string& request_id,
                                   const std::string& type,
                                   const std::string& field = "",
                                   const std::string& value = "") {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    auto addStr = [&](const std::string& k, const std::string& s) {
        JsonValue v; v.type = JsonValue::Type::String; v.str = s;
        root.obj.emplace_back(k, std::move(v));
    };
    addStr("v", kWorkerProtocolVersion);
    addStr("request_id", request_id);
    addStr("type", type);
    if (!field.empty()) addStr(field, value);
    return serializeJson(root) + "\n";
}

/**
 * 解析单行 worker 消息 (R4 合同: 与 Python protocol.validate 一致)。
 *  - 未知字段容忍 (解析后按需取字段)
 *  - hello 为握手例外: 可无 v / request_id
 *  - 其余消息: v 必须为 "1" (缺失/错误 → bad_version);
 *    request_id 必须非空 (→ bad_request_id);
 *    type 必须已知 (未知 → unknown_type, 结构化错误, 不再静默忽略)
 *  - 截断/非法 JSON → 结构化 protocol_error (绝不把残留当新消息)
 */
inline WorkerParseResult parseWorkerLine(const std::string& line) {
    // R4-R1 (D4): 完整行在进入 JSON parser 前按 UTF-8 字节检查长度
    if (line.size() > kMaxWorkerLineBytes) {
        return WorkerParseResult::fail("line_too_long", 0);
    }
    // R4-R1 (D4): 非法 UTF-8 → bad_json (与 Python 同策略)
    if (!isValidUtf8(line)) {
        return WorkerParseResult::fail("bad_json", 0);
    }
    ProtocolError err;
    auto root = parseJson(line, &err);
    if (!root) {
        // R4: 稳定错误码 (诊断细节保留在 mini_json, 合同只认 bad_json)
        return WorkerParseResult::fail("bad_json", err.pos);
    }
    if (root->type != JsonValue::Type::Object) {
        return WorkerParseResult::fail("not_object", 0);
    }
    WorkerMessage m;
    if (auto s = root->getString("request_id")) m.request_id = *s;
    if (auto s = root->getString("type"))        m.type = *s;
    if (auto s = root->getString("value"))       m.value = *s;
    if (auto s = root->getString("text"))        m.text = *s;
    if (auto s = root->getString("code"))        m.code = *s;   // R8_R2_R6 (A)
    if (auto n = root->getNumber("rms"))         m.rms = (*n < 0.0) ? -*n : *n;

    static const char* kKnown[] = {
        "start_turn", "state", "transcript", "tts_level",
        "done", "error", "cancel", "expression", "hello",
        "tts_started", "tts_finished", "tool_call", "stop_recording",
        "content", "response_complete",         // R4: 完整回答分片与结束标记
        "tool_request",                         // R8_R4: agentd 工具请求 (text=完整请求 JSON)
        "waiting"                               // R8_R4_R2: 慢回答等待提示 (text=提示文本)
    };
    bool known = false;
    for (const char* k : kKnown) {
        if (m.type == k) { known = true; break; }
    }
    if (!known) {
        return WorkerParseResult::fail("unknown_type", 0);
    }
    m.known_type = true;

    // R4-R1 (D3): C++ 客户端只收事件 — 请求专属类型反向出现 → wrong_direction
    // (transcript/cancel 双向合法, 不在此列)
    if (m.type == "start_turn" || m.type == "stop_recording") {
        return WorkerParseResult::fail("wrong_direction", 0);
    }

    // R4: 版本与 request_id 合同 (hello 豁免)
    if (m.type != "hello") {
        auto v = root->getString("v");
        if (!v || *v != kWorkerProtocolVersion) {
            return WorkerParseResult::fail("bad_version", 0);
        }
        if (m.request_id.empty()) {
            return WorkerParseResult::fail("bad_request_id", 0);
        }
    }
    return WorkerParseResult::ok(std::move(m));
}

} // namespace holopet
