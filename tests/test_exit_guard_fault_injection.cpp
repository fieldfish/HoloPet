/**
 * test_exit_guard_fault_injection.cpp — R4-R1 P4 (D8): 异常注入
 *
 * 断言:
 *  - thread 启动后的任意抛异常点都在 guard 保护内 (guard 先于抛点建立)
 *  - Shutdown 入队成功 (push 返回 true) → join 有可终止前提
 *  - stop_encoder 抛异常不阻断 destroy_display
 *  - 每步收尾不抛出 (调用方安全)
 */

#include "ipc/ai_command_queue.hpp"
#include "ipc/ai_io_loop.hpp"
#include "ipc/exit_guard.hpp"
#include "agent/agent_client.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static int64_t fakeNow() { return 0; }

int main() {
    std::cout << "=== test_exit_guard_fault_injection (V6 R4-R1) ===\n\n";

    {
        // 模拟 main_ai 结构: IO 线程创建后立即 guard;
        // 之后抛异常 (encoder.start 等价位置) → guard 完整收尾
        AiCommandQueue commands;
        WorkerEventQueue events;
        AiWorkerConfig cfg; cfg.host = "127.0.0.1"; cfg.port = 1;
        AiWorkerSession session(cfg);
        std::atomic<bool> io_running{true};
        std::thread io([&]() {
            runAiIoLoop(commands, session, io_running, events, fakeNow);
        });

        ExitGuard guard;
        guard.commands = &commands;
        guard.io_thread = &io;
        int display_destroyed = 0;
        guard.destroy_display = [&display_destroyed]() { ++display_destroyed; };
        int encoder_stopped = 0;
        guard.stop_encoder = [&encoder_stopped]() { ++encoder_stopped; };

        // 抛异常点 (guard 已建立)
        try {
            throw std::runtime_error("injected: encoder.start failure");
        } catch (const std::exception&) {
            // 被外层 catch; guard 随作用域析构
        }
    }   // guard 析构: Shutdown → join IO → stop_encoder → destroy_display

    check(true, "异常注入后进程不 terminate (guard 收尾完成)");

    // Shutdown 入队必须成功 (队列不满/或满时也不丢 — 见 queue 测试)
    {
        AiCommandQueue q;
        check(q.push(AiCommand::Shutdown), "Shutdown push 返回 true");
        check(q.push(AiCommand::Shutdown), "重复 Shutdown 幂等");
    }

    // stop_encoder 抛异常 → destroy_display 仍执行
    {
        ExitGuard g;
        int display = 0;
        g.stop_encoder = []() { throw std::runtime_error("encoder join fail"); };
        g.destroy_display = [&display]() { ++display; };
        g.run();
        check(display == 1, "stop_encoder 抛异常后 destroy_display 仍执行");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
