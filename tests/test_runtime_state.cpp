/**
 * test_runtime_state.cpp — V6 运行状态/情绪枚举测试
 */

#include "agent/agent_event.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_runtime_state (V6) ===\n\n";

    check(std::string(runtimeStateName(RuntimeState::Idle)) == "Idle", "Idle name");
    check(std::string(runtimeStateName(RuntimeState::Listening)) == "Listening", "Listening name");
    check(std::string(runtimeStateName(RuntimeState::Thinking)) == "Thinking", "Thinking name");
    check(std::string(runtimeStateName(RuntimeState::Speaking)) == "Speaking", "Speaking name");
    check(std::string(runtimeStateName(RuntimeState::Error)) == "Error", "Error name");
    check(std::string(runtimeStateName(RuntimeState::Offline)) == "Offline", "Offline name");

    check(std::string(emotionName(Emotion::Neutral)) == "Neutral", "Neutral name");
    check(std::string(emotionName(Emotion::Happy)) == "Happy", "Happy name");
    check(std::string(emotionName(Emotion::Curious)) == "Curious", "Curious name");
    check(std::string(emotionName(Emotion::Surprised)) == "Surprised", "Surprised name");
    check(std::string(emotionName(Emotion::Sad)) == "Sad", "Sad name");
    check(std::string(emotionName(Emotion::Sleepy)) == "Sleepy", "Sleepy name");
    check(std::string(emotionName(Emotion::Concerned)) == "Concerned", "Concerned name");

    check(static_cast<int>(RuntimeState::Offline) ==
          static_cast<int>(RuntimeState::Error) + 1, "State enum order stable");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
