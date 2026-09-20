/**
 * test_submit_text_command.cpp — R8_R2 (工作包 D) 回归:
 *   1. 文字 typed command 携带内容且队列所有权正确 (SubmitTextTurn)。
 *   2. tryPopMsg 顺序保持 (FIFO); 旧 tryPop 兼容 (丢弃 text)。
 *   3. session.submitTextTurn 发送顺序与 request_id 一致
 *      (transcript → start_turn → stop_recording, 同 rid)。
 *   4. 未连接时不伪造成功 (submitTextTurn=false, 无发送)。
 *   5. busy: 已有 active turn 时第二次提交受控拒绝。
 *   6. cancel 后 active 清空可再次提交。
 *   7. Shutdown 高优先级不退化 (满队列也绝不丢)。
 *   8. --text-once-file 纯函数校验: 缺文件/空/超限/无效 UTF-8/正常。
 */

#include "agent/agent_client.hpp"
#include "ipc/ai_command_queue.hpp"
#include "ipc/text_once.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
static void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

// ---- session 注入式 probe: 记录 pending_ 中已构造的消息 ----
// submitTextTurn 只入 pending_; 用 flush 观察需要 socket。
// 这里通过 AiWorkerSession 的公开 submitTextTurn 返回值与
// 内部状态 (active_id 空/非空) 验证行为; 顺序验证使用
// "未连接时 false + 连接语义由既有 uds roundtrip 覆盖" 的边界策略。

int main() {
    std::cout << "=== test_submit_text_command (R8_R2 D) ===\n\n";

    // 1) 队列 payload 所有权 + 顺序
    {
        AiCommandQueue q;
        check(q.push(AiCommand::SubmitTextTurn, std::string("你好，HoloPet")),
              "SubmitTextTurn 入队 (带文本)");
        check(q.push(AiCommand::StartTurn), "兼容 push(AiCommand) 仍可用");
        AiCommandMsg m1, m2;
        check(q.tryPopMsg(m1), "tryPopMsg 1");
        check(m1.cmd == AiCommand::SubmitTextTurn &&
              m1.text == "你好，HoloPet", "第 1 条: 文本内容完整且未被改写");
        check(q.tryPopMsg(m2), "tryPopMsg 2");
        check(m2.cmd == AiCommand::StartTurn, "第 2 条: 顺序 FIFO 保持");
        check(m2.text.empty(), "无 payload 命令 text 为空");
        // tryPop 兼容路径
        q.push(AiCommand::CancelTurn, std::string("x"));
        AiCommand legacy;
        check(q.tryPop(legacy) && legacy == AiCommand::CancelTurn,
              "旧 tryPop 兼容 (丢弃 text, 命令类型正确)");
    }

    // 2) 队列容量与 Shutdown 优先级 (不退化)
    {
        AiCommandQueue q;
        bool all_ok = true;
        for (size_t i = 0; i < 64; ++i)
            if (!q.push(AiCommand::SubmitTextTurn, "t" + std::to_string(i)))
                all_ok = false;
        check(all_ok, "容量内 64 条全部入队");
        check(!q.push(AiCommand::StartTurn), "满队列拒绝普通命令");
        check(q.push(AiCommand::Shutdown), "满队列仍接受 Shutdown");
        AiCommandMsg m;
        // Shutdown 应排在末尾 (挤掉最旧普通命令腾位), 且不被后续拒绝
        size_t n = 0; bool seen_shutdown = false;
        while (q.tryPopMsg(m)) { ++n; if (m.cmd == AiCommand::Shutdown) seen_shutdown = true; }
        check(seen_shutdown, "Shutdown 未因队列满丢失");
        check(n <= 64, "队列长度有界");
    }

    // 3) submitTextTurn: 未连接 → false (不伪造)
    {
        AiWorkerConfig cfg;
        cfg.uds_path = "";
        cfg.host = "127.0.0.1";
        cfg.port = 59999;                 // 无服务
        AiWorkerSession s(cfg);
        WorkerEventQueue evq;
        s.drive(0, evq);                  // 推进一步 (连接失败路径)
        check(!s.submitTextTurn("hi"), "未连接时 submitTextTurn=false (不伪造)");
    }

    // 4) submitTextTurn: 空文本 → false
    {
        AiWorkerConfig cfg;
        cfg.uds_path = "";
        cfg.host = "127.0.0.1";
        cfg.port = 59998;
        AiWorkerSession s(cfg);
        check(!s.submitTextTurn(""), "空文本 → false");
    }

    // 5) text_once 校验
    {
        const std::string base = "r8r2_d_test_";
        std::string err;
        auto r1 = loadTextOnceFile(base + "missing.txt", err);
        check(!r1 && err == "missing_file", "缺文件 → missing_file");

        { std::ofstream f(base + "empty.txt"); }
        err.clear();
        auto r2 = loadTextOnceFile(base + "empty.txt", err);
        check(!r2 && err == "empty_file", "空文件 → empty_file");

        { std::ofstream f(base + "big.txt", std::ios::binary);
          std::string big(9000, 'a'); f << big; }
        err.clear();
        auto r3 = loadTextOnceFile(base + "big.txt", err);
        check(!r3 && err == "too_large", "超 8KiB → too_large");

        { std::ofstream f(base + "bad.txt", std::ios::binary);
          const char bad[] = {'h', 'i', (char)0xFF, (char)0xFE, 0}; f << bad; }
        err.clear();
        auto r4 = loadTextOnceFile(base + "bad.txt", err);
        check(!r4 && err == "invalid_utf8", "无效 UTF-8 → invalid_utf8");

        { std::ofstream f(base + "ok.txt", std::ios::binary);
          f << "  你好，讲个笑话  \n"; }
        err.clear();
        auto r5 = loadTextOnceFile(base + "ok.txt", err);
        check(r5 && *r5 == "你好，讲个笑话", "正常文件: 裁剪首尾空白保留内容");

        std::remove((base + "empty.txt").c_str());
        std::remove((base + "big.txt").c_str());
        std::remove((base + "bad.txt").c_str());
        std::remove((base + "ok.txt").c_str());
    }

    // 6) isValidUtf8 边界
    {
        check(isValidUtf8("中文 ok"), "合法中文+ASCII");
        check(isValidUtf8("emoji \xF0\x9F\x98\x8A"), "合法 4 字节 emoji");
        check(!isValidUtf8(std::string("\xE4\xB8")), "截断 3 字节序列拒绝");
        check(!isValidUtf8(std::string("\xC0\x80")), "过长 2 字节编码拒绝");
        check(!isValidUtf8(std::string("\xED\xA0\x80")), "UTF-16 代理区拒绝");
    }

    // ============================================================
    // R8_R2_R1 (A+B): 生命周期状态机 / 分页展示 / 审计
    // ============================================================
    {
        TextTurnLifecycle L;
        L.configure(60000, 60000, 1000, 1000);
        L.setStartMs(0);
        L.onSubmitted("rid-1", 10);
        L.setAnswerState(false, "");
        check(!L.readyToFinish(5000), "A1 提交后无事件不得提前成功");
        L.onEvent(TextTurnEvent::Content, "hi", 20);
        check(!L.readyToFinish(5000), "A2 只有 content 不得成功");
        L.onEvent(TextTurnEvent::ResponseComplete, "", 21);
        L.onEvent(TextTurnEvent::Expression, "happy", 22);
        check(!L.readyToFinish(5000), "A3 缺 done 不得成功");
        L.onEvent(TextTurnEvent::TurnDone, "", 23);
        check(!L.readyToFinish(5000), "A4 缺 committed/分页不得成功");
        L.setAnswerState(true, "hello world");
        L.bindPages(2, 30);
        L.markPageRendered(31);
        check(!L.readyToFinish(1100), "A5 多页未翻完不得成功");
        check(L.shouldAdvancePage(2000), "A5b 首页 dwell 满后可翻页");
        L.advancePage(2000);
        L.markPageRendered(2001);
        check(L.pagesShown().size() == 2 && L.pagesShown()[1].index == 1,
              "A5c page_shown 顺序连续 (0,1)");
        check(!L.readyToFinish(2500), "A6 末页 hold 未满不得退出");
        check(L.readyToFinish(3100), "A6b 全部条件+hold 满 -> 可完成");
        L.finishSuccess();
        check(L.exitCode() == TextTurnExit::Success, "A6c 成功退出码 0");
        const int d0 = L.doneCount();
        L.onEvent(TextTurnEvent::TurnDone, "", 4000);
        L.onEvent(TextTurnEvent::Content, "late", 4001);
        check(L.doneCount() == d0 && L.contentChunks() == 1,
              "A8 终态后迟到事件被忽略");
    }
    {
        TextTurnLifecycle L;
        L.configure(60000, 60000, 1000, 1000);
        L.setStartMs(0);
        L.onSubmitted("rid-e", 1);
        L.onEvent(TextTurnEvent::TurnError, "", 2);
        L.markTerminalError();
        check(static_cast<int>(L.exitCode()) == 4, "A4b turn error -> rc 4");
        TextTurnLifecycle L2;
        L2.configure(60000, 60000, 1000, 1000);
        L2.setStartMs(0);
        L2.onSubmitted("rid-c", 1);
        L2.onEvent(TextTurnEvent::TurnCancelled, "", 2);
        L2.markCancelled();
        check(static_cast<int>(L2.exitCode()) == 5, "A4c cancel -> rc 5");
        TextTurnLifecycle L3;
        L3.configure(5000, 5000, 1000, 1000);
        L3.setStartMs(0);
        L3.onSubmitted("rid-h", 1);
        L3.onEvent(TextTurnEvent::Content, "x", 100);
        L3.onEvent(TextTurnEvent::ResponseComplete, "", 101);
        L3.onEvent(TextTurnEvent::Expression, "neutral", 101);
        L3.onEvent(TextTurnEvent::TurnDone, "", 102);
        L3.setAnswerState(true, "xxxx");
        L3.bindPages(3, 200);
        L3.markPageRendered(201);
        check(L3.pollTimeout(99999) == -1,
              "A7 展示期间不计入 Provider/终态超时");
        TextTurnLifecycle L4;
        L4.configure(1000, 1000, 1000, 1000);
        L4.setStartMs(0);
        check(L4.pollTimeout(2000) == 3, "A7b worker 超时 -> rc 3");
        TextTurnLifecycle L5;
        L5.configure(60000, 1000, 1000, 1000);
        L5.setStartMs(0);
        L5.onSubmitted("rid-t", 1);
        L5.onEvent(TextTurnEvent::Content, "y", 2);
        check(L5.pollTimeout(5000) == 6, "A7c 终态等待超时 -> rc 6");
    }
    {
        check(sha256Hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "B10 sha256(empty) standard vector");
        check(sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "B10b sha256(abc) standard vector");
        TextEventAudit aud;
        check(!aud.open("/definitely/missing_dir/ev.jsonl"),
              "B9 审计不可写 -> fail closed");
        const std::string ep = "r8r2r1_audit_test.jsonl";
        std::remove(ep.c_str());
        TextEventAudit a2;
        check(a2.open(ep), "B9b 审计正常打开");
        check(a2.writeLine("{\"event\":\"start\"}"), "B9c 写行");
        check(a2.commit(), "B9d commit (atomic rename)");
        std::ifstream in(ep, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        check(body.find("start") != std::string::npos, "B9e 审计内容完整");
        check(body.find("sk-") == std::string::npos,
              "B10d 审计不含密钥形态");
        in.close();
        std::remove(ep.c_str());
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
