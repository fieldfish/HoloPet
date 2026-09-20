/**
 * test_command_queue_shutdown_priority.cpp — R4-R1 P4 (D8):
 * Shutdown 高优先级, 队列满也永不丢
 */

#include "ipc/ai_command_queue.hpp"

#include <iostream>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_command_queue_shutdown_priority (V6 R4-R1) ===\n\n";

    {
        AiCommandQueue q;
        // 填满普通命令
        int enqueued = 0;
        while (q.push(AiCommand::CancelTurn)) ++enqueued;
        check(enqueued == 64, "普通命令填满 64 上限 (多余被拒)");
        check(q.size() == 64, "队列大小 64");
        // 满时 Shutdown 仍必须入队 (挤掉最旧普通命令)
        check(q.push(AiCommand::Shutdown), "满队列 Shutdown 入队成功");
        // 队列内有 Shutdown
        bool found = false;
        AiCommand c;
        while (q.tryPop(c)) {
            if (c == AiCommand::Shutdown) found = true;
        }
        check(found, "Shutdown 未被丢 (被消费)");
    }
    {
        AiCommandQueue q;
        q.push(AiCommand::Shutdown);
        check(q.push(AiCommand::Shutdown), "重复 Shutdown 幂等合并");
        AiCommand c;
        int n = 0;
        while (q.tryPop(c)) ++n;
        check(n == 1, "合并后只有一个 Shutdown");
    }
    {
        // 满队列 + 新 Shutdown: 挤掉最旧的 CancelTurn, 保留后面的 StopRecording
        AiCommandQueue q;
        while (q.push(AiCommand::CancelTurn)) {}
        q.push(AiCommand::Shutdown);
        AiCommand first;
        q.tryPop(first);
        check(first == AiCommand::CancelTurn, "Shutdown 未抢占队首 (仅保证不丢)");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
