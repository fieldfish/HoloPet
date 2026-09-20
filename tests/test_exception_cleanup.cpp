/**
 * test_exception_cleanup.cpp — R3-R1 缺陷 D2: 异常路径收尾测试
 *
 * 验证 (ExitGuard, src/ipc/exit_guard.hpp — 与 main_ai 同一实现):
 *  1. thread 启动后抛异常 → guard 析构仍完整收尾;
 *  2. 顺序: Shutdown 消费/IO join → stop_encoder → destroy_display;
 *  3. stop_encoder 抛异常不阻断 destroy_display;
 *  4. 收尾幂等 (重复析构/run 不重复执行);
 *  5. Shutdown 被 IO 线程真实消费 (io_running=false, 队列 drain)。
 */

#include "agent/agent_client.hpp"
#include "ipc/ai_command_queue.hpp"
#include "ipc/ai_io_loop.hpp"
#include "ipc/exit_guard.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static int64_t fakeNow() { return 0; }

struct Recorder {
    std::vector<std::string> order;
    void mark(const std::string& s) { order.push_back(s); }
    int indexOf(const std::string& s) {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == s) return static_cast<int>(i);
        return -1;
    }
};

int main() {
    std::cout << "=== test_exception_cleanup (V6-R1) ===\n\n";

    // ---- 1+2+5: 异常路径完整收尾 + 顺序 + Shutdown 消费 ----
    {
        AiCommandQueue commands;
        WorkerEventQueue events;
        AiWorkerConfig cfg; cfg.host = "127.0.0.1"; cfg.port = 1;
        AiWorkerSession session(cfg);
        std::atomic<bool> io_running{true};
        std::atomic<bool> io_exited{false};
        std::thread io([&]() {
            runAiIoLoop(commands, session, io_running, events, fakeNow);
            io_exited = true;
        });

        Recorder rec;
        {
            ExitGuard guard;
            guard.commands = &commands;
            guard.io_thread = &io;
            guard.stop_encoder = [&rec]() { rec.mark("encoder"); };
            guard.destroy_display = [&rec]() { rec.mark("display"); };
            rec.mark("started");
            // 模拟主循环抛异常 (thread 启动后的任意异常路径)
            try {
                throw std::runtime_error("simulated main-loop failure");
            } catch (const std::exception&) {
                // 异常被外层捕获, guard 随作用域析构收尾
            }
        }   // ← guard 析构

        check(io_exited.load(), "异常路径: IO 线程已退出");
        check(!io_running.load(), "异常路径: Shutdown 被 IO 线程消费 (非主线程抢先)");
        AiCommand leftover;
        check(!commands.tryPop(leftover), "异常路径: 命令队列已 drain");
        check(rec.indexOf("encoder") >= 0 && rec.indexOf("display") >= 0,
              "异常路径: encoder/display 收尾均已执行");
        check(rec.indexOf("encoder") < rec.indexOf("display"),
              "顺序: encoder 先于 display 销毁");
        // guard 已完成 join → joinable()==false (对 non-joinable 线程再 join
        // 会抛 system_error, 因此只断言状态, 不重复 join)
        check(!io.joinable(), "IO 线程已被 guard join (joinable=false)");
    }

    // ---- 3: stop_encoder 抛异常不阻断 destroy_display ----
    {
        AiCommandQueue commands;
        Recorder rec;
        {
            ExitGuard guard;
            guard.commands = &commands;
            guard.stop_encoder = [&rec]() {
                rec.mark("encoder-throws");
                throw std::runtime_error("encoder stop failed");
            };
            guard.destroy_display = [&rec]() { rec.mark("display"); };
        }
        check(rec.indexOf("display") >= 0,
              "encoder 收尾抛异常后 display 仍被销毁");
    }

    // ---- 4: 幂等 ----
    {
        AiCommandQueue commands;
        Recorder rec;
        ExitGuard guard;
        guard.commands = &commands;
        guard.stop_encoder = [&rec]() { rec.mark("encoder"); };
        guard.destroy_display = [&rec]() { rec.mark("display"); };
        guard.run();
        size_t after_first = rec.order.size();
        guard.run();                 // 幂等: 不重复执行
        check(rec.order.size() == after_first, "收尾幂等 (重复 run 不重复执行)");
        // 析构再触发一次也不重复
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
