/**
 * test_hardware_pipeline.cpp — V6 硬件集成语义测试 (V6-R2)
 * 模拟 EncoderEvent 进入同一 InputEventQueue → drain → ConversationController:
 * Idle 短按 → StartTurn/Listening; Listening 短按 → StopRecording; 长按取消; 旋转音量。
 */

#include "agent/conversation_controller.hpp"
#include "input/input_event.hpp"
#include "input/input_queue.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_hardware_pipeline (V6-R2) ===\n\n";

    InputEventQueue queue;               // 与真实 EC11/键盘共用的同一队列
    ConversationController controller;

    // Idle 短按 → StartTurn + Listening (状态闭环: 不自行 Thinking)
    queue.push(EncoderEvent::Pressed);
    EncoderEvent ev;
    check(queue.tryPop(ev) && ev == EncoderEvent::Pressed, "事件经同一队列");
    auto cmd = controller.handleEncoder(ev, 0);
    check(cmd == ConversationController::Command::StartTurn, "Idle 短按 → StartTurn");
    check(controller.state() == RuntimeState::Listening,
          "状态保持 Listening (等 worker 事件, 不自行 Thinking)");

    // Listening 短按 → StopRecording
    cmd = controller.handleEncoder(EncoderEvent::Pressed, 100);
    check(cmd == ConversationController::Command::StopRecording, "Listening 短按 → StopRecording");

    // 长按取消 → Idle
    cmd = controller.handleEncoder(EncoderEvent::LongPressed, 200);
    check(cmd == ConversationController::Command::CancelTurn, "长按 → CancelTurn");
    check(controller.state() == RuntimeState::Idle, "取消后 Idle");

    // 旋转音量 (经队列)
    float v0 = controller.volume();
    queue.push(EncoderEvent::Clockwise);
    queue.tryPop(ev);
    cmd = controller.handleEncoder(ev, 300);
    check(cmd == ConversationController::Command::None, "旋转无命令");
    check(controller.volume() > v0, "顺时针音量上升");

    queue.push(EncoderEvent::CounterClockwise);
    queue.tryPop(ev);
    controller.handleEncoder(ev, 300);
    check(controller.volume() == v0, "逆时针音量回落");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
