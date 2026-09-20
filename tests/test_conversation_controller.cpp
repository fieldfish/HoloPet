/**
 * test_conversation_controller.cpp — V6 生产对话控制器 (EC11 行为/事件/音量) 测试
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
    std::cout << "=== test_conversation_controller (V6) ===\n\n";

    ConversationController c;
    check(c.state() == RuntimeState::Idle, "初始 Idle");
    check(c.emotion() == Emotion::Neutral, "初始 Neutral");

    // 短按 Idle → Listening + StartTurn
    auto cmd = c.handleEncoder(EncoderEvent::Pressed, 0);
    check(cmd == ConversationController::Command::StartTurn, "Idle 短按 → StartTurn");
    check(c.state() == RuntimeState::Listening, "状态 → Listening");

    // 短按 Listening → Thinking + StopRecording
    cmd = c.handleEncoder(EncoderEvent::Pressed, 100);
    check(cmd == ConversationController::Command::StopRecording, "Listening 短按 → StopRecording");

    // 长按 → 取消 + 回 Idle
    cmd = c.handleEncoder(EncoderEvent::LongPressed, 200);
    check(cmd == ConversationController::Command::CancelTurn, "长按 → CancelTurn");
    check(c.state() == RuntimeState::Idle, "取消后 Idle");

    // 旋转 → 音量
    float v0 = c.volume();
    cmd = c.handleEncoder(EncoderEvent::Clockwise, 300);
    check(cmd == ConversationController::Command::None, "旋转无命令");
    check(c.volume() > v0, "顺时针音量上升");
    check(c.volumeLayerVisible(310), "音量层可见 (层内)");
    check(!c.volumeLayerVisible(310 + 2000), "音量层超时隐藏");
    for (int i = 0; i < 40; ++i) c.handleEncoder(EncoderEvent::Clockwise, 0);
    check(c.volume() == 1.0f, "音量上限夹取 1.0");

    // Agent 事件: worker online/offline
    AgentEvent on; on.type = AgentEventType::WorkerOnline;
    check(c.handleAgent(on), "WorkerOnline 有变化");
    AgentEvent off; off.type = AgentEventType::WorkerLost;
    c.handleAgent(off);
    check(c.state() == RuntimeState::Offline, "WorkerLost → Offline");

    // Offline 短按尝试开始 (命令发出, 主程序判定 worker 是否可用)
    cmd = c.handleEncoder(EncoderEvent::Pressed, 0);
    check(cmd == ConversationController::Command::StartTurn, "Offline 短按仍发 StartTurn");
    check(c.state() == RuntimeState::Listening, "状态 → Listening");

    // StateChanged (worker 驱动)
    AgentEvent st; st.type = AgentEventType::StateChanged;
    st.state = RuntimeState::Thinking;
    c.handleAgent(st);
    check(c.state() == RuntimeState::Thinking, "StateChanged → Thinking");

    // 超时
    c.markThinking(0);
    check(!c.tick(20000), "20s 未超时");
    check(!c.tick(30001), "30s+ 未超时 (R8_R3_R3_R1)");
    check(c.tick(210001), "最终超时");
    check(c.state() == RuntimeState::Error, "超时 → Error");

    // Error 可恢复 (TurnDone)
    AgentEvent done; done.type = AgentEventType::TurnDone;
    c.handleAgent(done);
    check(c.state() == RuntimeState::Idle, "TurnDone 恢复 Idle");

    // Transcript 记录
    AgentEvent tr; tr.type = AgentEventType::Transcript; tr.text = "你好";
    c.handleAgent(tr);
    check(c.userTranscript() == "你好", "Transcript 进入用户文本 (与回答分离)");

    // 情绪独立更新 (Expression 事件)
    AgentEvent ex; ex.type = AgentEventType::Expression; ex.emotion = Emotion::Happy;
    c.handleAgent(ex);
    check(c.emotion() == Emotion::Happy, "情绪独立更新");
    check(c.state() == RuntimeState::Idle, "情绪变化不改状态 (正交)");

// ---- R8_R3_R3_R1 (修复二): Thinking 超时不得凭空制造 Error ----
{
    ConversationController c2;
    c2.markThinking(0);
    c2.tick(29000);   check(c2.state() == RuntimeState::Thinking, "Thinking 29s 仍 Thinking");
    c2.tick(30000);   check(c2.state() == RuntimeState::Thinking, "Thinking 30s 仍 Thinking");
    c2.tick(80000);   check(c2.state() == RuntimeState::Thinking, "Thinking 80s 仍 Thinking");
    c2.tick(210001);  check(c2.state() == RuntimeState::Error, "最终超时 → Error");
    AgentEvent done; done.type = AgentEventType::TurnDone;
    c2.handleAgent(done, 220000);
    check(c2.state() == RuntimeState::Idle, "TurnDone → Idle");
}
{
    ConversationController c3;
    c3.markThinking(0);
    AgentEvent err; err.type = AgentEventType::TurnError;
    c3.handleAgent(err, 1000);
    check(c3.state() == RuntimeState::Error, "worker TurnError 立即 Error");
}
{
    ConversationController c4;
    c4.markThinking(0);
    AgentEvent content; content.type = AgentEventType::ContentChunk;
    content.text = "回答";
    c4.handleAgent(content, 200000);
    check(c4.state() == RuntimeState::Thinking, "超时前收到 content 不切 Error");
    AgentEvent resp; resp.type = AgentEventType::ResponseComplete;
    c4.handleAgent(resp, 200001);
    check(c4.state() == RuntimeState::Thinking, "response_complete 后仍不假 Error");
    AgentEvent done2; done2.type = AgentEventType::TurnDone;
    c4.handleAgent(done2, 200002);
    check(c4.state() == RuntimeState::Idle, "done → Idle");
}
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}

