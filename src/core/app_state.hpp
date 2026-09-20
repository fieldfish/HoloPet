#pragma once
/**
 * app_state.hpp — App 状态机 (V1.0.2 实现)
 */

namespace holopet {

enum class AppState {
    Idle,
    Listening,
    Thinking,
    Speaking,
    Error
};

inline const char* appStateName(AppState s) {
    switch (s) {
        case AppState::Idle:      return "Idle";
        case AppState::Listening: return "Listening";
        case AppState::Thinking:  return "Thinking";
        case AppState::Speaking:  return "Speaking";
        case AppState::Error:     return "Error";
    }
    return "???";
}

class AppStateManager {
public:
    void setState(AppState s) { state_ = s; }
    AppState getState() const { return state_; }
    const char* stateName() const { return appStateName(state_); }

private:
    AppState state_ = AppState::Idle;
};

} // namespace holopet
