// test_tool_wire.cpp — R8_R4 工具请求线级协议测试 (纯逻辑, 无 SDL)。
#include <iostream>
#include <string>

#include "agent/agent_event.hpp"
#include "ipc/ai_worker_client.hpp"
#include "ipc/ai_worker_protocol.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_tool_wire (R8_R4) ===\n\n";

    {
        // 1. tool_request 为已知类型, 携带完整请求 JSON 于 text
        const std::string line =
            "{\"v\":\"1\",\"request_id\":\"r1\",\"type\":\"tool_request\","
            "\"text\":\"{\\\"v\\\":\\\"1\\\",\\\"request_id\\\":\\\"r1\\\","
            "\\\"tool_call_id\\\":\\\"c1\\\",\\\"name\\\":\\\"create_timer\\\","
            "\\\"arguments\\\":{\\\"duration_ms\\\":\\\"60000\\\"}}\"}";
        auto res = parseWorkerLine(line);
        check(!res.error && res.message && res.message->known_type,
              "tool_request 解析为已知消息");
        check(res.message->type == "tool_request", "类型字段正确");
        check(res.message->text.find("create_timer") != std::string::npos,
              "text 携带完整请求 JSON");

        auto ev = toAgentEvent(*res.message);
        check(ev && ev->type == AgentEventType::ToolRequest,
              "映射为 ToolRequest 事件");
        check(ev->text.find("tool_call_id") != std::string::npos
              && ev->text.find("duration_ms") != std::string::npos,
              "事件 text 含 tool_call_id 与参数");
    }

    {
        // 2. 非法 tool_request (缺 text) 仍为已知类型但 text 为空 (上层拒绝)
        const std::string line =
            "{\"v\":\"1\",\"request_id\":\"r2\",\"type\":\"tool_request\"}";
        auto res = parseWorkerLine(line);
        check(!res.error && res.message->known_type, "无 text 不崩 (上层拒绝)");
        check(res.message->text.empty(), "text 为空");
    }

    {
        // 3. 未知类型仍被结构化拒绝 (回归保障)
        const std::string line =
            "{\"v\":\"1\",\"request_id\":\"r3\",\"type\":\"sudo_rm\"}";
        auto res = parseWorkerLine(line);
        check(res.error != std::nullopt, "未知类型 → 结构化拒绝");
    }

    {
        // 4. R8_R4_R2: waiting 为已知类型, 映射 Waiting 事件并透传提示文本
        const std::string line =
            "{\"v\":\"1\",\"request_id\":\"r4\",\"type\":\"waiting\","
            "\"text\":\"这个问题要多想一会儿，正在认真找答案\"}";
        auto res = parseWorkerLine(line);
        check(!res.error && res.message && res.message->known_type,
              "waiting 解析为已知消息");
        check(res.message->type == "waiting", "类型字段正确");
        auto ev = toAgentEvent(*res.message);
        check(ev && ev->type == AgentEventType::Waiting,
              "映射为 Waiting 事件");
        check(ev->text.find("多想一会儿") != std::string::npos,
              "提示文本透传");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
