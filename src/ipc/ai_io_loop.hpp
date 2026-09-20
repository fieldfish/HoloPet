#pragma once
/**
 * ai_io_loop.hpp — IO owner 线程主循环 (V6-R3)
 *
 * R3 修正 (相对 R2 main_ai.cpp 内联循环):
 *  - Shutdown 语义: 主线程只入队, 不再抢先置 io_running=false;
 *    由 IO 线程消费 Shutdown 命令后自行退出 → 排队的 cancel/shutdown
 *    保证被消费 (R2 竞态: 主线程先置标志, IO 线程可能带着未消费命令退出)。
 *  - 循环抽成独立函数: main 与 tests/test_shutdown_queue_drain 共用同一段
 *    生产代码, 退出语义有测试门。
 *
 * 仍保持 R2 架构: 本循环是 AiWorkerSession 的唯一所有者; SDL 主线程
 * 只投递 AiCommand, 绝不直接调用 session。
 */

#include "agent/agent_client.hpp"
#include "ipc/ai_command_queue.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace holopet {

/**
 * IO owner 线程主循环。
 * @param commands       主线程 → IO 线程命令队列 (唯一消费方)
 * @param session        worker 会话 (本线程独占)
 * @param io_running     运行标志; 仅由本循环 (消费 Shutdown 后) 置 false
 * @param worker_events  worker 事件出口 (→ SDL 主线程)
 * @param now_ms         单调时钟 (毫秒)
 */
inline void runAiIoLoop(AiCommandQueue& commands, AiWorkerSession& session,
                        std::atomic<bool>& io_running,
                        WorkerEventQueue& worker_events,
                        const std::function<int64_t()>& now_ms) {
    while (io_running.load()) {
        AiCommandMsg msg;
        while (commands.tryPopMsg(msg)) {
            switch (msg.cmd) {
                case AiCommand::StartTurn:
                    if (session.status().connected)
                        session.startTurn(msg.meta.empty()
                                          ? "auto" : msg.meta);
                    break;
                case AiCommand::StopRecording:
                    session.stopRecording();
                    break;
                case AiCommand::CancelTurn:
                    session.cancelTurn();
                    break;
                case AiCommand::SubmitTextTurn:
                    // R8_R2 (D): 文字轮 — busy/未连接/空文本时不发 (不伪造)
                    (void)session.submitTextTurn(
                        msg.text, msg.meta.empty() ? "auto" : msg.meta);
                    break;
                case AiCommand::SendRaw:
                    session.sendRawLine(msg.text);
                    break;
                case AiCommand::Shutdown:
                    // 消费方确认: 已 drain 到 Shutdown, 排在其前的取消
                    // 语义已执行; 退出标志只在这里置位 (主线程不抢先)。
                    io_running.store(false);
                    break;
            }
        }
        session.drive(now_ms(), worker_events);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    session.shutdown();
}

} // namespace holopet
