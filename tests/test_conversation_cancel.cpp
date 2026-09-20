/**
 * test_conversation_cancel.cpp — V6 取消/停止/离线路径测试
 */

#include "agent/conversation_controller.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_conversation_cancel (V6) ===\n\n";

    // Speaking 短按 → 停止播放
    ConversationController c;
    AgentEvent st; st.type = AgentEventType::StateChanged;
    st.state = RuntimeState::Speaking;
    c.handleAgent(st);
    auto cmd = c.handleEncoder(EncoderEvent::Pressed, 0);
    check(cmd == ConversationController::Command::StopSpeak, "Speaking 短按 → StopSpeak");
    check(c.state() == RuntimeState::Idle, "停止后 Idle");

    // Thinking 短按无动作
    st.state = RuntimeState::Thinking;
    c.handleAgent(st);
    cmd = c.handleEncoder(EncoderEvent::Pressed, 0);
    check(cmd == ConversationController::Command::None, "Thinking 短按无动作");

    // 长按取消 Thinking
    cmd = c.handleEncoder(EncoderEvent::LongPressed, 0);
    check(cmd == ConversationController::Command::CancelTurn, "Thinking 长按 → CancelTurn");
    check(c.state() == RuntimeState::Idle, "取消后 Idle");

    // Idle 长按无动作
    cmd = c.handleEncoder(EncoderEvent::LongPressed, 0);
    check(cmd == ConversationController::Command::None, "Idle 长按无动作");

    // TurnError → Error (R4-R1: 错误文本不再冒充回答; pending 丢弃, 已提交保留)
    AgentEvent err; err.type = AgentEventType::TurnError; err.text = "boom";
    c.handleAgent(err);
    check(c.state() == RuntimeState::Error, "TurnError → Error");
    check(c.pendingAnswer().empty(), "TurnError 丢弃未提交回答");

    // TurnCancelled → Idle
    AgentEvent canc; canc.type = AgentEventType::TurnCancelled;
    c.handleAgent(canc);
    check(c.state() == RuntimeState::Idle, "TurnCancelled → Idle");

    // 音量下限
    for (int i = 0; i < 40; ++i) c.handleEncoder(EncoderEvent::CounterClockwise, 0);
    check(c.volume() == 0.0f, "音量下限夹取 0.0");

    // Worker 断开后再恢复
    AgentEvent lost; lost.type = AgentEventType::WorkerLost;
    c.handleAgent(lost);
    AgentEvent back; back.type = AgentEventType::WorkerOnline;
    c.handleAgent(back);
    check(c.state() == RuntimeState::Idle, "Offline → WorkerOnline 回 Idle");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
