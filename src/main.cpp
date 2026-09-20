/**
 * main.cpp — HoloPet DSV4 V1 入口
 *
 * V1 只负责: 初始化 → 运行集成验证流程 → 退出
 * 不包含业务逻辑、算法、硬件驱动。
 */

#include "core/app_state.hpp"
#include "input/encoder.hpp"
#include "system/logger.hpp"

#include <iostream>
#include <cstdlib>

using namespace holopet;

int main() {
    LOG_INFO("Main") << "HoloPet DSV4 V1 starting ...";

    // ====== 1. AppState 验证 ======
    LOG_INFO("Main") << "--- AppState ---";
    AppStateManager app;
    LOG_INFO("Main") << "initial: " << app.stateName();

    app.setState(AppState::Listening);
    LOG_INFO("Main") << "→ " << app.stateName();

    app.setState(AppState::Thinking);
    LOG_INFO("Main") << "→ " << app.stateName();

    app.setState(AppState::Speaking);
    LOG_INFO("Main") << "→ " << app.stateName();

    app.setState(AppState::Error);
    LOG_INFO("Main") << "→ " << app.stateName();

    app.setState(AppState::Idle);
    LOG_INFO("Main") << "→ " << app.stateName();

    // ====== 2. QuadratureDecoder 验证 ======
    LOG_INFO("Main") << "--- QuadratureDecoder ---";
    {
        QuadratureDecoder dec;
        // 首帧初始化
        dec.update(false, false, 0);

        // 完整正转一圈: 00→01→11→10→00
        dec.update(false, true,  100);
        dec.update(true,  true,  200);
        dec.update(true,  false, 300);
        auto ev = dec.update(false, false, 400);
        if (ev.has_value()) {
            LOG_INFO("Main") << "CW detent detected: "
                             << (ev.value() == EncoderEvent::Clockwise ? "Clockwise" : "?");
        }
    }

    // ====== 3. ButtonDecoder 验证 ======
    LOG_INFO("Main") << "--- ButtonDecoder ---";
    {
        ButtonDecoder btn;
        // 短按: 按下 → 释放 → 消抖确认
        btn.update(false, 0);
        btn.update(true,  100'000);
        btn.update(false, 200'000);
        auto ev = btn.update(false, 300'000);  // 消抖完成后返回值
        if (ev.has_value() && ev.value() == EncoderEvent::Pressed) {
            LOG_INFO("Main") << "short press: Pressed";
        } else {
            LOG_WARN("Main") << "short press: no Pressed detected";
        }
    }

    // ====== 4. InputEventQueue 验证 ======
    LOG_INFO("Main") << "--- InputEventQueue ---";
    {
        InputEventQueue q;
        q.push(EncoderEvent::Clockwise);
        q.push(EncoderEvent::Pressed);
        EncoderEvent out;
        while (q.tryPop(out)) {
            const char* name = "?";
            switch (out) {
                case EncoderEvent::Clockwise:        name = "Clockwise"; break;
                case EncoderEvent::CounterClockwise: name = "CCW"; break;
                case EncoderEvent::Pressed:          name = "Pressed"; break;
                case EncoderEvent::LongPressed:      name = "LongPressed"; break;
            }
            LOG_INFO("Main") << "queued event: " << name;
        }
    }

    LOG_INFO("Main") << "HoloPet V1 demo completed.";
    LOG_INFO("Main") << "Run ctest for verification.";
    return 0;
}
