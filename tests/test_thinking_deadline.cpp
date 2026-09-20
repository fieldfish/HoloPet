/**
 * test_thinking_deadline.cpp — R4-R1 P1 (D2): Thinking 超时锚点
 *
 * 断言:
 *  - 程序运行 100000 ms 后进入 Thinking 不会立即超时
 *  - 只从"本次进入"起经过 turn_timeout_ms 才 Error
 *  - 编码器路径 (Listening 短按 → markThinking) 与 worker 路径
 *    (StateChanged Thinking) 都锚定; 重复 Thinking 事件不重置锚点
 */

#include "agent/conversation_controller.hpp"

#include <iostream>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_thinking_deadline (V6 R4-R1) ===\n\n";

    const int64_t kUptime = 100000;   // 程序已运行 100 秒
    const int64_t kTimeout = 210000;  // R8_R3_R3_R1: 30s→210s

    // ---- 1. worker 路径: 100s 后进入 Thinking ----
    {
        ConversationController c;
        AgentEvent st; st.type = AgentEventType::StateChanged;
        st.state = RuntimeState::Thinking;
        c.handleAgent(st, kUptime);              // 进入时刻锚定
        check(!c.tick(kUptime + 1), "100s 后进入 Thinking, 1ms 不超时");
        check(!c.tick(kUptime + kTimeout - 1), "209.999s 不超时");
        check(c.tick(kUptime + kTimeout), "210.000s 超时 → Error");
        check(c.state() == RuntimeState::Error, "状态 Error");
    }

    // ---- 2. 编码器路径: Listening 短按锚定 ----
    {
        ConversationController c;
        c.handleEncoder(EncoderEvent::Pressed, kUptime);   // Idle → Listening
        auto cmd = c.handleEncoder(EncoderEvent::Pressed, kUptime + 10);
        check(cmd == ConversationController::Command::StopRecording,
              "Listening 短按 → StopRecording");
        check(c.state() == RuntimeState::Thinking, "进入 Thinking");
        check(!c.tick(kUptime + 10 + kTimeout - 1), "编码器路径 209.999s 不超时");
        check(c.tick(kUptime + 10 + kTimeout), "编码器路径 210s 超时");
    }

    // ---- 3. 重复 Thinking 不重置锚点 (固定策略) ----
    {
        ConversationController c;
        AgentEvent st; st.type = AgentEventType::StateChanged;
        st.state = RuntimeState::Thinking;
        c.handleAgent(st, kUptime);
        c.handleAgent(st, kUptime + 25000);     // 对端重复发 Thinking
        check(!c.tick(kUptime + 209000), "重复事件后原锚点仍生效 (209s 未超时)");
        check(c.tick(kUptime + kTimeout), "重复事件不推迟超时 (210s 超时)");
    }

    // ---- 4. 未锚定时不得误判 (旧缺陷: 初始 0 锚点) ----
    {
        ConversationController c;
        AgentEvent st; st.type = AgentEventType::StateChanged;
        st.state = RuntimeState::Thinking;
        c.handleAgent(st);                      // 无 now_ms (旧调用方式)
        check(!c.tick(1000000), "未锚定时 tick 不得用 0 做基准误判超时");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
