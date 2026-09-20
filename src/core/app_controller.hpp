#pragma once
/**
 * app_controller.hpp — AppController: EncoderEvent → AppState (V4)
 *
 * 纯 C++ 逻辑。禁止 include SDL / gpiod / thread。
 * 仅由主线程访问，无需 mutex。
 */

#include "core/app_state.hpp"
#include "input/input_event.hpp"

namespace holopet {

class AppController {
public:
    AppController() = default;

    /** 当前状态 */
    AppState state() const noexcept { return state_; }

    /**
     * 处理一个 EncoderEvent
     * @return true  = 状态发生了变化
     *         false = 状态未变
     */
    bool handleEvent(EncoderEvent ev) {
        switch (ev) {
        case EncoderEvent::Clockwise: {
            int idx = static_cast<int>(state_);
            AppState next = static_cast<AppState>((idx + 1) % kStateCount);
            if (next != state_) {
                state_ = next;
                return true;
            }
            return false;
        }
        case EncoderEvent::CounterClockwise: {
            int idx = static_cast<int>(state_);
            AppState next = static_cast<AppState>((idx + kStateCount - 1) % kStateCount);
            if (next != state_) {
                state_ = next;
                return true;
            }
            return false;
        }
        case EncoderEvent::Pressed:
            // 确认当前状态，不变化
            return false;

        case EncoderEvent::LongPressed:
            if (state_ != AppState::Idle) {
                state_ = AppState::Idle;
                return true;
            }
            return false;
        }
        return false;
    }

private:
    static constexpr int kStateCount = 5;
    AppState state_ = AppState::Idle;
};

} // namespace holopet
