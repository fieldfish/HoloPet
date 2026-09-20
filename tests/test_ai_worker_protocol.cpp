/**
 * test_ai_worker_protocol.cpp — V6 IPC 协议解析测试 (未知字段容忍/转义)
 */

#include "ipc/ai_worker_protocol.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_ai_worker_protocol (V6) ===\n\n";

    // 协议版本
    check(std::string(kWorkerProtocolVersion) == "1", "协议版本 = 1");

    // 转义 (R2: jsonEscape 返回完整 JSON 字符串字面量, 含包裹引号)
    check(jsonEscape("he\"llo") == "\"he\\\"llo\"", "引号转义");
    check(jsonEscape("a\\b") == "\"a\\\\b\"", "反斜杠转义");
    check(jsonEscape("x\ny") == "\"x\\ny\"", "换行转义");

    // 构造
    auto line = buildOutMessage("rid-1", "start_turn");
    check(line.find("\"v\":\"1\"") != std::string::npos, "out 含协议版本");
    check(line.find("\"request_id\":\"rid-1\"") != std::string::npos, "out 含 request_id");
    check(line.find("\"type\":\"start_turn\"") != std::string::npos, "out 含 type");
    check(!line.empty() && line.back() == '\n', "out 以换行结尾 (JSON Lines)");

    auto line2 = buildOutMessage("r2", "transcript", "text", "你\"好");
    check(line2.find("\\\"") != std::string::npos, "字段值转义进入消息");

    // 解析 (R4 合同: v + request_id 必填, hello 豁免)
    auto r1 = parseWorkerLine(
        R"({"v":"1","request_id":"abc","type":"transcript","text":"你好"})");
    check(r1.message.has_value() && !r1.error.has_value(), "transcript 解析成功");
    check(r1.message->request_id == "abc" && r1.message->type == "transcript" &&
          r1.message->text == "你好", "transcript 字段");
    check(r1.message->known_type, "known_type = true");

    // 未知字段容忍 (多一个 future 字段不影响)
    auto r2 = parseWorkerLine(
        R"({"v":"1","request_id":"x","type":"done","future_field":123,"nested":{"a":1}})");
    check(r2.message && r2.message->type == "done" && r2.message->known_type,
          "未知字段容忍");

    // 未知 type → R4 结构化错误 (不再 known_type=false 静默)
    auto r3 = parseWorkerLine(
        R"({"v":"1","request_id":"x","type":"mystery","text":"?"})");
    check(!r3.message && r3.error && r3.error->reason == "unknown_type",
          "未知 type → structured unknown_type");

    // 数字字段
    auto r4 = parseWorkerLine(
        R"({"v":"1","request_id":"x","type":"tts_level","rms":0.42})");
    check(r4.message && r4.message->rms > 0.41 && r4.message->rms < 0.43, "rms 解析");

    // 空白容忍
    auto r5 = parseWorkerLine(
        R"( { "v" : "1" , "request_id" : "y" , "type" : "state" , "value" : "thinking" } )");
    check(r5.message && r5.message->type == "state" && r5.message->value == "thinking",
          "空白容忍解析");

    // 缺 request_id → R4 结构化 bad_request_id
    auto r6 = parseWorkerLine(R"({"v":"1","type":"error"})");
    check(!r6.message && r6.error && r6.error->reason == "bad_request_id",
          "缺 request_id → bad_request_id");

    // 缺 v / 错误 v → bad_version
    auto r6b = parseWorkerLine(R"({"request_id":"x","type":"done"})");
    check(!r6b.message && r6b.error && r6b.error->reason == "bad_version",
          "缺 v → bad_version");
    auto r6c = parseWorkerLine(R"({"v":"2","request_id":"x","type":"done"})");
    check(!r6c.message && r6c.error && r6c.error->reason == "bad_version",
          "v=2 → bad_version");

    // hello 豁免 v/request_id
    auto r6d = parseWorkerLine(R"({"type":"hello"})");
    check(r6d.message && r6d.message->type == "hello",
          "hello 豁免 v/request_id");

    // R4 新类型
    auto r6e = parseWorkerLine(
        R"({"v":"1","request_id":"x","type":"content","text":"片段一"})");
    check(r6e.message && r6e.message->known_type, "content 类型已知");
    auto r6f = parseWorkerLine(
        R"({"v":"1","request_id":"x","type":"response_complete"})");
    check(r6f.message && r6f.message->known_type, "response_complete 类型已知");

    // 坏 JSON → 结构化协议错误 (稳定码 bad_json, 不崩溃)
    auto r7 = parseWorkerLine("{bad json");
    check(!r7.message.has_value() && r7.error.has_value() &&
          r7.error->reason == "bad_json", "坏 JSON → structured bad_json");
    auto r8 = parseWorkerLine("[1,2]");
    check(!r8.message && r8.error && r8.error->reason == "not_object",
          "非 object → not_object");
    auto r9 = parseWorkerLine("");
    check(!r9.message && r9.error && r9.error->reason == "bad_json",
          "空行 → bad_json");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
