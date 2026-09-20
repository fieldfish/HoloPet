/**
 * test_app_state.cpp — AppState 状态机 + InputEventQueue 测试
 *
 * 测试: 状态切换 / 状态名 / 事件队列 push/pop 顺序
 */

#include "core/app_state.hpp"
#include "input/input_queue.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace holopet;

static int g_passed = 0;
static int g_failed = 0;

void check(bool cond, const char* test_name) {
    if (cond) {
        std::cout << "  PASS: " << test_name << '\n';
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << test_name << '\n';
        ++g_failed;
    }
}

int main() {
    std::cout << "=== test_app_state ===\n\n";

    // ========== AppState 部分 ==========

    // ---- 状态名测试 ----
    std::cout << "[状态名]\n";
    check(std::string(appStateName(AppState::Idle)) == "Idle", "Idle → \"Idle\"");
    check(std::string(appStateName(AppState::Listening)) == "Listening", "Listening → \"Listening\"");
    check(std::string(appStateName(AppState::Thinking)) == "Thinking", "Thinking → \"Thinking\"");
    check(std::string(appStateName(AppState::Speaking)) == "Speaking", "Speaking → \"Speaking\"");
    check(std::string(appStateName(AppState::Error)) == "Error", "Error → \"Error\"");

    // ---- 状态管理器测试 ----
    std::cout << "\n[AppStateManager]\n";

    AppStateManager mgr;
    check(mgr.getState() == AppState::Idle, "初始状态 = Idle");
    check(std::string(mgr.stateName()) == "Idle", "初始 stateName() = \"Idle\"");

    mgr.setState(AppState::Listening);
    check(mgr.getState() == AppState::Listening, "Idle → Listening");

    mgr.setState(AppState::Thinking);
    check(mgr.getState() == AppState::Thinking, "Listening → Thinking");

    mgr.setState(AppState::Speaking);
    check(mgr.getState() == AppState::Speaking, "Thinking → Speaking");

    mgr.setState(AppState::Error);
    check(mgr.getState() == AppState::Error, "Speaking → Error");

    mgr.setState(AppState::Idle);
    check(mgr.getState() == AppState::Idle, "Error → Idle");

    // 完整遍历
    std::cout << "\n[完整状态遍历 Idle→Listening→Thinking→Speaking→Error]\n";
    AppStateManager mgr2;
    AppState expected[] = {
        AppState::Idle, AppState::Listening, AppState::Thinking,
        AppState::Speaking, AppState::Error
    };
    bool all_ok = true;
    for (int i = 0; i < 5; ++i) {
        mgr2.setState(expected[i]);
        if (mgr2.getState() != expected[i]) all_ok = false;
    }
    check(all_ok, "顺序遍历全部5个状态通过");

    // ========== InputEventQueue 部分 ==========

    std::cout << "\n[InputEventQueue 基本 push/pop]\n";
    {
        InputEventQueue q;
        EncoderEvent out;

        // 空队列 pop 失败
        check(!q.tryPop(out), "空队列 tryPop → false");

        // push → pop 顺序一致
        q.push(EncoderEvent::Clockwise);
        check(q.tryPop(out) && out == EncoderEvent::Clockwise, "push CW → pop CW");

        q.push(EncoderEvent::CounterClockwise);
        q.push(EncoderEvent::Pressed);
        q.push(EncoderEvent::LongPressed);

        check(q.tryPop(out) && out == EncoderEvent::CounterClockwise, "FIFO: 第1个 CCW");
        check(q.tryPop(out) && out == EncoderEvent::Pressed,          "FIFO: 第2个 Pressed");
        check(q.tryPop(out) && out == EncoderEvent::LongPressed,      "FIFO: 第3个 LongPressed");
        check(!q.tryPop(out), "全部取出后为空");
    }

    // ---- 线程安全测试 ----
    std::cout << "\n[InputEventQueue 线程安全]\n";
    {
        InputEventQueue q;
        constexpr int N = 1000;
        std::vector<EncoderEvent> received;
        std::mutex recv_mutex;

        // 生产者线程
        std::thread producer([&]() {
            for (int i = 0; i < N; ++i) {
                q.push(EncoderEvent::Clockwise);
            }
        });

        // 消费者线程
        std::thread consumer([&]() {
            EncoderEvent ev;
            int count = 0;
            while (count < N) {
                if (q.tryPop(ev)) {
                    std::lock_guard lock(recv_mutex);
                    received.push_back(ev);
                    ++count;
                }
            }
        });

        producer.join();
        consumer.join();

        check(received.size() == static_cast<size_t>(N), "线程安全: 收到 N 个事件");
        bool all_cw = true;
        for (auto& ev : received) {
            if (ev != EncoderEvent::Clockwise) { all_cw = false; break; }
        }
        check(all_cw, "线程安全: 所有事件 = Clockwise");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
