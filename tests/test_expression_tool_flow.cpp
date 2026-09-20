/**
 * test_expression_tool_flow.cpp — R4-R1 P1 (D10): set_expression → 情绪生效
 *
 * 断言:
 *  - tool_call=set_expression (happy) 后 expression=happy 到达 C++ Emotion::Happy
 *  - 未知情绪值回落 neutral (双语: 中文/英文值)
 *  - 情绪独立于运行状态 (Speaking + Happy 可并存)
 */

#include "agent/agent_event.hpp"
#include "agent/conversation_controller.hpp"
#include "ipc/ai_worker_client.hpp"
#include "ipc/ai_worker_protocol.hpp"

#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

/** 模拟 worker 发来 expression 事件 */
static Emotion feedExpression(ConversationController& c, const std::string& value) {
    auto m = parseWorkerLine(
        "{\"v\":\"1\",\"request_id\":\"r\",\"type\":\"expression\",\"value\":\"" +
        value + "\"}");
    auto ev = toAgentEvent(*m.message);
    c.handleAgent(*ev);
    return c.emotion();
}

int main() {
    std::cout << "=== test_expression_tool_flow (V6 R4-R1) ===\n\n";

    ConversationController c;

    // ---- 1. set_expression happy → expression=happy → Emotion::Happy ----
    check(feedExpression(c, "happy") == Emotion::Happy,
          "expression=happy → C++ Emotion::Happy");

    // ---- 2. 白名单情绪逐一映射 ----
    check(feedExpression(c, "curious") == Emotion::Curious, "curious → Curious");
    check(feedExpression(c, "surprised") == Emotion::Surprised, "surprised → Surprised");
    check(feedExpression(c, "sad") == Emotion::Sad, "sad → Sad");
    check(feedExpression(c, "sleepy") == Emotion::Sleepy, "sleepy → Sleepy");
    check(feedExpression(c, "concerned") == Emotion::Concerned, "concerned → Concerned");
    check(feedExpression(c, "neutral") == Emotion::Neutral, "neutral → Neutral");

    // ---- 3. 未知情绪回落 neutral (双语单测) ----
    check(feedExpression(c, "excited") == Emotion::Neutral, "未知值 excited → Neutral");
    check(feedExpression(c, "高兴") == Emotion::Neutral, "未知中文值 高兴 → Neutral");
    // R8_R4_R3_R4_R5: 九表情新名称映射; 真正未知值仍回退 Neutral
    check(feedExpression(c, "angry") == Emotion::Angry, "angry → Angry");
    check(feedExpression(c, "craving") == Emotion::Craving, "craving → Craving");
    check(feedExpression(c, "whatever") == Emotion::Neutral,
          "未知值 whatever → Neutral");

    // ---- 4. 情绪独立于状态 ----
    feedExpression(c, "happy");
    AgentEvent st; st.type = AgentEventType::StateChanged;
    st.state = RuntimeState::Speaking;
    c.handleAgent(st);
    check(c.state() == RuntimeState::Speaking && c.emotion() == Emotion::Happy,
          "Speaking 与 Happy 并存 (状态/情绪正交)");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
