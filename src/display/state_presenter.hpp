#pragma once
/**
 * state_presenter.hpp — AppState → 显示信息映射 (V3)
 *
 * 纯 C++ 逻辑, 禁止 include SDL / gpiod。
 * 可独立单元测试。
 */

#include "core/app_state.hpp"
#include <string>

namespace holopet {

struct StateViewModel {
    std::string title;
    std::string subtitle;
};

inline StateViewModel present(AppState state) {
    switch (state) {
        case AppState::Idle:
            return {"HoloPet", "Ready"};
        case AppState::Listening:
            return {"Listening", "I'm listening..."};
        case AppState::Thinking:
            return {"Thinking", "Working on it..."};
        case AppState::Speaking:
            return {"Speaking", "Answering..."};
        case AppState::Error:
            return {"Error", "Something went wrong"};
    }
    return {"Unknown", ""};  // 防御性返回
}

} // namespace holopet
