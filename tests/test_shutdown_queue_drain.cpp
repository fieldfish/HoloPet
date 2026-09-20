/**
 * test_shutdown_queue_drain.cpp — R3 工作包 C 退出语义测试
 *
 * 验证 (对应 R2 缺陷: 主线程入队 Shutdown 后立即置 io_running=false,
 * IO 线程可能带着未消费的 cancel/shutdown 退出):
 *  1. Shutdown 入队后, IO 线程确实消费 CancelTurn + Shutdown 再退出;
 *  2. io_running 只由消费方 (IO 线程) 置 false;
 *  3. 退出后命令队列已 drain (无遗留);
 *  4. 重复 Shutdown 幂等;
 *  5. 无命令时循环可被 Shutdown 正常终止 (不挂起)。
 *
 * 使用与 main_ai.cpp 相同的生产循环 runAiIoLoop (ipc/ai_io_loop.hpp)。
 */

#include "agent/agent_client.hpp"
#include "ipc/ai_command_queue.hpp"
#include "ipc/ai_io_loop.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static int64_t fakeNow() { return 0; }

/** 跑一轮完整退出: 入队 cmds → 起 IO 线程 → 等 drain → 返回耗时 ms */
static bool runShutdownCycle(AiCommandQueue& commands,
                             std::initializer_list<AiCommand> pre,
                             int timeout_ms = 5000) {
    AiWorkerConfig cfg;                 // 指向不可达端口 — drive 只做非阻塞尝试
    cfg.host = "127.0.0.1";
    cfg.port = 1;                       // 保留端口, 连接必失败 (不依赖外部服务)
    AiWorkerSession session(cfg);
    WorkerEventQueue events;
    std::atomic<bool> io_running{true};
    std::atomic<int> consumed{0};

    for (auto c : pre) commands.push(c);
    commands.push(AiCommand::Shutdown);

    std::thread io([&]() {
        runAiIoLoop(commands, session, io_running, events, fakeNow);
        consumed.store(1);
    });

    auto t0 = std::chrono::steady_clock::now();
    while (!consumed.load()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > timeout_ms) {
            io.detach();                // 超时: 不得 join 挂死测试进程
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    io.join();
    return io_running.load() == false;
}

int main() {
    std::cout << "=== test_shutdown_queue_drain (V6-R3) ===\n\n";

    {
        AiCommandQueue q;
        check(runShutdownCycle(q, {AiCommand::CancelTurn, AiCommand::StopRecording}),
              "IO 线程消费 CancelTurn/StopRecording/Shutdown 后退出");
    }
    {
        AiCommandQueue q;
        check(runShutdownCycle(q, {}),
              "仅 Shutdown 也能正常终止循环");
    }
    {
        // drain 验证: 退出后队列必须为空
        AiCommandQueue q;
        bool ok = runShutdownCycle(q, {AiCommand::CancelTurn});
        AiCommand leftover;
        check(ok && !q.tryPop(leftover), "退出后命令队列已 drain (无遗留)");
    }
    {
        // 幂等: 连续多个 Shutdown
        AiCommandQueue q;
        check(runShutdownCycle(q, {AiCommand::Shutdown}),
              "重复 Shutdown 幂等退出");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
