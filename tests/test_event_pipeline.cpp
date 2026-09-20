/**
 * test_event_pipeline.cpp — 事件管道集成测试 (V4-R1)
 *
 * 纯 C++ 验证: Queue FIFO + AppController 串联。
 * 不启动 SDL / GPIO。
 */

#include "core/app_controller.hpp"
#include "input/input_queue.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0;
static int g_failed = 0;

void check(bool cond, const std::string& test_name) {
    if (cond) {
        std::cout << "  PASS: " << test_name << '\n';
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << test_name << '\n';
        ++g_failed;
    }
}

int main() {
    std::cout << "=== test_event_pipeline ===\n\n";

    // ---- 基本管线: CW CW CCW Pressed ----
    std::cout << "[基本管线: CW CW CCW Pressed]\n";
    {
        InputEventQueue queue;
        AppController controller;

        queue.push(EncoderEvent::Clockwise);        // Idle → Listening
        queue.push(EncoderEvent::Clockwise);        // Listening → Thinking
        queue.push(EncoderEvent::CounterClockwise); // Thinking → Listening
        queue.push(EncoderEvent::Pressed);           // 确认, 不变

        int changes = 0;
        EncoderEvent ev;
        while (queue.tryPop(ev)) {
            if (controller.handleEvent(ev)) ++changes;
        }

        check(controller.state() == AppState::Listening, "最终状态 = Listening");
        check(changes == 3, "CW CW CCW = 3 次变化 (Pressed 不变)");
    }

    // ---- LongPressed 回 Idle ----
    std::cout << "\n[LongPressed → Idle]\n";
    {
        InputEventQueue queue;
        AppController controller;

        queue.push(EncoderEvent::Clockwise);   // → Listening
        queue.push(EncoderEvent::Clockwise);   // → Thinking
        queue.push(EncoderEvent::LongPressed); // → Idle

        EncoderEvent ev;
        int changes = 0;
        while (queue.tryPop(ev)) {
            if (controller.handleEvent(ev)) ++changes;
        }

        check(controller.state() == AppState::Idle, "最终 = Idle");
        check(changes == 3, "CW CW Long → 3 次变化");
    }

    // ---- Idle + LongPressed 不变 ----
    std::cout << "\n[Idle + LongPressed 不变]\n";
    {
        InputEventQueue queue;
        AppController controller;

        queue.push(EncoderEvent::LongPressed);

        EncoderEvent ev;
        int changes = 0;
        while (queue.tryPop(ev)) {
            if (controller.handleEvent(ev)) ++changes;
        }

        check(controller.state() == AppState::Idle, "仍为 Idle");
        check(changes == 0, "无变化");
    }

    // ---- FIFO 顺序 ----
    std::cout << "\n[FIFO 顺序: 5×CW → 环绕回 Idle]\n";
    {
        InputEventQueue queue;
        AppController controller;

        for (int i = 0; i < 5; ++i) queue.push(EncoderEvent::Clockwise);

        int changes = 0;
        EncoderEvent ev;
        while (queue.tryPop(ev)) {
            if (controller.handleEvent(ev)) ++changes;
        }

        check(controller.state() == AppState::Idle, "环绕回 Idle");
        check(changes == 5, "5×CW → 5 次变化");
    }

    // ---- 混合序列 ----
    std::cout << "\n[混合序列: CW CW CCW Long CW CW]\n";
    {
        InputEventQueue queue;
        AppController controller;

        queue.push(EncoderEvent::Clockwise);        // Idle → Listening
        queue.push(EncoderEvent::Clockwise);        // → Thinking
        queue.push(EncoderEvent::CounterClockwise); // → Listening
        queue.push(EncoderEvent::LongPressed);      // → Idle
        queue.push(EncoderEvent::Clockwise);        // → Listening
        queue.push(EncoderEvent::Clockwise);        // → Thinking

        EncoderEvent ev;
        int changes = 0;
        while (queue.tryPop(ev)) {
            if (controller.handleEvent(ev)) ++changes;
        }

        check(controller.state() == AppState::Thinking, "最终 = Thinking");
        check(changes == 6, "6 次全部变化 (中间无 Pressed)");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
