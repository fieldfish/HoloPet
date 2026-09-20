#pragma once
/**
 * exit_guard.hpp — 线程启动后的统一退出收尾 (V6-R1)
 *
 * R3-R1 (缺陷 D2): 从 main_ai.cpp 匿名 namespace 抽出为可测试 owner。
 * 任何路径 (正常退出 / SIGINT / SDL_QUIT / 异常) 都经同一段收尾:
 *
 *   入队 Shutdown → join IO 线程 → 停 Encoder → 销毁显示(SDL)
 *
 * 顺序保证 (tests/test_exception_cleanup.cpp 验证):
 *   1. IO 线程 join 完成后才会 stop_encoder;
 *   2. 显示资源最后销毁;
 *   3. 收尾幂等;
 *   4. 某一步回调抛异常不阻断后续步骤;
 *   5. Shutdown 被 IO 线程真实消费 (与 ai_io_loop.hpp 语义配合)。
 *
 * main_ai.cpp 注入 SDL 专用 destroy_display;
 * 测试注入记录顺序的回调 — 同一实现, 不同注脚。
 */

#include "ipc/ai_command_queue.hpp"

#include <functional>
#include <thread>

namespace holopet {

class ExitGuard {
public:
    AiCommandQueue* commands = nullptr;
    std::thread* io_thread = nullptr;
    std::function<void()> stop_encoder;     // 无硬件时为空
    std::function<void()> destroy_display;  // SDL 销毁 (或测试记录器)
    bool done = false;

    // R4-R1 (D8): move-only — 复制会造成双重收尾 (重复 join/销毁),
    // 删除复制构造与复制赋值; 只允许显式移动或在作用域内构造。
    ExitGuard() = default;
    ExitGuard(const ExitGuard&) = delete;
    ExitGuard& operator=(const ExitGuard&) = delete;
    ExitGuard(ExitGuard&&) noexcept = default;
    ExitGuard& operator=(ExitGuard&&) noexcept = default;

    ~ExitGuard() { run(); }

    /** 执行收尾 (幂等); 析构自动调用。 */
    void run() {
        if (done) return;
        done = true;
        try {
            if (commands) commands->push(AiCommand::Shutdown);
        } catch (...) { /* 不抛出 */ }
        try {
            if (io_thread && io_thread->joinable()) io_thread->join();
        } catch (...) { /* 不抛出 */ }
        try {
            if (stop_encoder) stop_encoder();
        } catch (...) { /* 不抛出: 后续步骤仍须执行 */ }
        try {
            if (destroy_display) destroy_display();
        } catch (...) { /* 不抛出 */ }
    }
};

} // namespace holopet
