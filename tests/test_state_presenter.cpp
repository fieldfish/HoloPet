/**
 * test_state_presenter.cpp — StatePresenter 纯逻辑测试 (V3)
 *
 * 不启动 SDL。验证 5 个 AppState → ViewModel 映射。
 */

#include "display/state_presenter.hpp"
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
    std::cout << "=== test_state_presenter ===\n\n";

    // ---- 遍历 5 状态 ----
    std::cout << "[5 状态映射]\n";

    struct Expected { AppState state; const char* title; const char* subtitle; };
    const Expected cases[] = {
        {AppState::Idle,      "HoloPet",   "Ready"},
        {AppState::Listening, "Listening", "I'm listening..."},
        {AppState::Thinking,  "Thinking",  "Working on it..."},
        {AppState::Speaking,  "Speaking",  "Answering..."},
        {AppState::Error,     "Error",     "Something went wrong"},
    };

    bool all_title_nonempty = true;
    bool all_subtitle_nonempty = true;
    bool all_mapping_ok = true;

    for (const auto& c : cases) {
        auto vm = present(c.state);
        if (vm.title.empty())       all_title_nonempty = false;
        if (vm.subtitle.empty())    all_subtitle_nonempty = false;
        if (vm.title != c.title || vm.subtitle != c.subtitle) {
            all_mapping_ok = false;
            std::cout << "  FAIL mapping for "
                      << appStateName(c.state) << "\n";
        }
    }

    check(all_title_nonempty, "全部 5 状态 title 非空");
    check(all_subtitle_nonempty, "全部 5 状态 subtitle 非空");
    check(all_mapping_ok, "全部 5 状态映射正确");

    // ---- 逐状态详细 ----
    std::cout << "\n[逐状态验证]\n";
    {
        auto vm = present(AppState::Idle);
        check(vm.title == "HoloPet" && vm.subtitle == "Ready", "Idle: HoloPet / Ready");
    }
    {
        auto vm = present(AppState::Listening);
        check(vm.title == "Listening" && vm.subtitle == "I'm listening...",
              "Listening: Listening / I'm listening...");
    }
    {
        auto vm = present(AppState::Thinking);
        check(vm.title == "Thinking" && vm.subtitle == "Working on it...",
              "Thinking: Thinking / Working on it...");
    }
    {
        auto vm = present(AppState::Speaking);
        check(vm.title == "Speaking" && vm.subtitle == "Answering...",
              "Speaking: Speaking / Answering...");
    }
    {
        auto vm = present(AppState::Error);
        check(vm.title == "Error" && vm.subtitle == "Something went wrong",
              "Error: Error / Something went wrong");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
