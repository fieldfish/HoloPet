/**
 * test_app_controller.cpp — AppController 纯逻辑测试 (V4)
 *
 * 不启动 SDL / GPIO。
 */

#include "core/app_controller.hpp"
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
    std::cout << "=== test_app_controller ===\n\n";

    // ---- CW 前进 ----
    std::cout << "[Clockwise 前进]\n";
    {
        AppController ctl;
        check(ctl.state() == AppState::Idle, "初始 Idle");

        check(ctl.handleEvent(EncoderEvent::Clockwise), "CW → Listening (changed)");
        check(ctl.state() == AppState::Listening, "state = Listening");

        check(ctl.handleEvent(EncoderEvent::Clockwise), "CW → Thinking (changed)");
        check(ctl.state() == AppState::Thinking, "state = Thinking");

        ctl.handleEvent(EncoderEvent::Clockwise);  // → Speaking
        ctl.handleEvent(EncoderEvent::Clockwise);  // → Error
        ctl.handleEvent(EncoderEvent::Clockwise);  // → Idle
        check(ctl.state() == AppState::Idle, "5×CW → Idle (环绕)");
    }

    // ---- CCW 后退 ----
    std::cout << "\n[CounterClockwise 后退]\n";
    {
        AppController ctl;
        check(ctl.handleEvent(EncoderEvent::CounterClockwise), "CCW → Error (changed)");
        check(ctl.state() == AppState::Error, "state = Error (Idle-1)");

        ctl.handleEvent(EncoderEvent::CounterClockwise);  // → Speaking
        ctl.handleEvent(EncoderEvent::CounterClockwise);  // → Thinking
        check(ctl.state() == AppState::Thinking, "3×CCW → Thinking");
    }

    // ---- Pressed 不变化 ----
    std::cout << "\n[Pressed 不改变状态]\n";
    {
        AppController ctl;
        check(!ctl.handleEvent(EncoderEvent::Pressed), "Idle + Pressed → false (未变)");
        check(ctl.state() == AppState::Idle, "state 仍为 Idle");

        ctl.handleEvent(EncoderEvent::Clockwise);  // → Listening
        check(!ctl.handleEvent(EncoderEvent::Pressed), "Listening + Pressed → false");
        check(ctl.state() == AppState::Listening, "state 仍为 Listening");
    }

    // ---- LongPressed → Idle ----
    std::cout << "\n[LongPressed → Idle]\n";
    {
        AppController ctl;
        ctl.handleEvent(EncoderEvent::Clockwise);  // Idle → Listening
        ctl.handleEvent(EncoderEvent::Clockwise);  // → Thinking

        check(ctl.handleEvent(EncoderEvent::LongPressed), "Thinking + LongPressed → Idle (changed)");
        check(ctl.state() == AppState::Idle, "state = Idle");

        // 已经 Idle 再长按不应变化
        check(!ctl.handleEvent(EncoderEvent::LongPressed), "Idle + LongPressed → false");
        check(ctl.state() == AppState::Idle, "state 仍为 Idle");
    }

    // ---- 任意状态 LongPressed ----
    std::cout << "\n[任意非Idle + LongPressed → Idle]\n";
    {
        AppState test_states[] = {
            AppState::Listening, AppState::Thinking,
            AppState::Speaking, AppState::Error
        };
        for (auto st : test_states) {
            AppController ctl;
            // 转到目标状态
            while (ctl.state() != st) ctl.handleEvent(EncoderEvent::Clockwise);
            check(ctl.handleEvent(EncoderEvent::LongPressed),
                  std::string(appStateName(st)) + " + LongPressed → Idle (changed)");
            check(ctl.state() == AppState::Idle, "state = Idle");
        }
    }

    // ---- 事件序列: CW×2 + CCW + Pressed ----
    std::cout << "\n[事件序列]\n";
    {
        AppController ctl;
        ctl.handleEvent(EncoderEvent::Clockwise);        // Idle → Listening
        ctl.handleEvent(EncoderEvent::Clockwise);        // → Thinking
        ctl.handleEvent(EncoderEvent::CounterClockwise); // → Listening
        ctl.handleEvent(EncoderEvent::Pressed);           // 确认
        check(ctl.state() == AppState::Listening, "CW CW CCW Pressed → Listening");

        ctl.handleEvent(EncoderEvent::LongPressed);       // → Idle
        check(ctl.state() == AppState::Idle, "LongPressed → Idle");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
