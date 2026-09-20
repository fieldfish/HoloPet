/**
 * test_text_turn_lifecycle.cpp — R8_R2_R1 (工作包 A+B) 专项回归:
 *   文字单轮生命周期状态机 (TextTurnLifecycle) + 原子事件审计
 *   (TextEventAudit) + SHA-256 摘要。
 *
 * 与 test_submit_text_command 的分工:
 *   - 该文件聚焦 typed command / 队列所有权 / IPC 边界;
 *   - 本文件聚焦"以真实终态判定成功"的十条成功条件、退出码表、
 *     分页逐页展示证据 (page_shown) 与审计 fail-closed, 并给出
 *     独立进程内的 CTest 证据 (AI/SDL 套件)。
 */

#include "ipc/text_once.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
static void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

namespace fs = std::filesystem;

// 一个"完整成功轮"的最小驱动器: 事件序列固定, 页数可调。
struct Turn {
    TextTurnLifecycle lc;
    explicit Turn(int pages = 1, int64_t page_ms = 1000,
                  int64_t hold_ms = 800) {
        lc.configure(5000, 5000, page_ms, hold_ms);
        lc.setStartMs(0);
        lc.onSubmitted("rid-0001", 10);
        lc.onEvent(TextTurnEvent::State, "thinking", 20);
        lc.onEvent(TextTurnEvent::Content, "你好", 30);
        lc.onEvent(TextTurnEvent::ResponseComplete, "", 40);
        lc.onEvent(TextTurnEvent::Expression, "happy", 45);
        lc.onEvent(TextTurnEvent::TurnDone, "", 50);
        lc.setAnswerState(true, "你好");
        if (pages > 0) lc.bindPages(pages, 60);
        lc.markPageRendered(62);
    }
};

int main() {
    std::cout << "=== test_text_turn_lifecycle (R8_R2_R1 A+B) ===\n\n";

    // ---- 1) 退出码表 (文档/脚本同表) ----
    {
        check(static_cast<int>(TextTurnExit::Success) == 0, "退出码: 成功=0");
        check(static_cast<int>(TextTurnExit::InvalidInput) == 2,
              "退出码: 输入无效=2");
        check(static_cast<int>(TextTurnExit::WorkerTimeout) == 3,
              "退出码: worker 超时=3");
        check(static_cast<int>(TextTurnExit::TurnError) == 4,
              "退出码: turn/协议错误=4");
        check(static_cast<int>(TextTurnExit::Cancelled) == 5,
              "退出码: 取消=5");
        check(static_cast<int>(TextTurnExit::TerminalTimeout) == 6,
              "退出码: 终态等待超时=6");
        check(static_cast<int>(TextTurnExit::PageOrAuditFail) == 7,
              "退出码: 页面/审计失败=7");
    }

    // ---- 2) 计时边界: worker 等待超时 vs 终态超时 (互不串台) ----
    {
        TextTurnLifecycle lc;
        lc.configure(1000, 4000, 1000, 800);
        lc.setStartMs(0);
        check(lc.phase() == TextTurnPhase::WaitingWorker,
              "初始阶段 = WaitingWorker");
        check(lc.pollTimeout(1000) == -1, "等待 worker: 未超时不退出");
        check(lc.pollTimeout(1001) == 3,
              "等待 worker 超时 → 3 (不是终态超时 6)");
        lc.onSubmitted("rid-x", 1001);
        check(lc.phase() == TextTurnPhase::Submitted, "提交后阶段 = Submitted");
        check(lc.pollTimeout(1500) == -1, "已提交: worker 超时不适用");
        lc.onEvent(TextTurnEvent::Content, "a", 1100);
        check(lc.phase() == TextTurnPhase::TurnActive,
              "观察到本轮活动 → TurnActive");
        check(lc.pollTimeout(4000) == -1, "终态超时 4000 内不退出");
        check(lc.pollTimeout(4001) == 6,
              "等待真实终态超时 → 6 (不是 worker 超时 3)");
        check(lc.readyToFinish(9999) == false, "超时后不得判定成功");
    }

    // ---- 3) 十条成功条件: 逐条缺失都必须 fail ----
    {
        Turn ok;
        check(ok.lc.readyToFinish(61) == false,
              "条件⑨: final hold 未满 → 不完成");
        check(ok.lc.finalHoldSatisfied(62 + 799) == false,
              "final hold 差 1ms 仍不满足");
        check(ok.lc.finalHoldSatisfied(62 + 800), "final hold 到点即满足");
        check(ok.lc.readyToFinish(62 + 800), "完整轮: 全部条件满足 → 可完成");
        check(ok.lc.exitCode() == TextTurnExit::TerminalTimeout,
              "未显式结束前退出码保持未定值 (非 0)");
        check(ok.lc.requestId() == "rid-0001", "已提交: request_id 回读");

        TextTurnLifecycle a;
        a.configure(1000, 1000, 100, 100);
        a.setStartMs(0);
        a.onSubmitted("", 10);   // 空 request_id
        a.onEvent(TextTurnEvent::Content, "x", 20);
        a.onEvent(TextTurnEvent::ResponseComplete, "", 30);
        a.onEvent(TextTurnEvent::Expression, "neutral", 40);
        a.onEvent(TextTurnEvent::TurnDone, "", 50);
        a.setAnswerState(true, "x");
        a.bindPages(1, 60);
        a.markPageRendered(61);
        check(a.readyToFinish(9999) == false,
              "条件①: request_id 为空 → 不完成");

        TextTurnLifecycle b;
        b.configure(1000, 1000, 100, 100);
        b.setStartMs(0);
        b.onSubmitted("rid-b", 10);
        b.onEvent(TextTurnEvent::ResponseComplete, "", 20);
        b.onEvent(TextTurnEvent::Expression, "happy", 30);
        b.onEvent(TextTurnEvent::TurnDone, "", 40);
        b.setAnswerState(true, "answer");
        b.bindPages(1, 50);
        b.markPageRendered(51);
        check(b.readyToFinish(9999) == false,
              "条件③: 无 content 分片 → 不完成");

        TextTurnLifecycle c;
        c.configure(1000, 1000, 100, 100);
        c.setStartMs(0);
        c.onSubmitted("rid-c", 10);
        c.onEvent(TextTurnEvent::Content, "answer", 15);
        c.onEvent(TextTurnEvent::Expression, "happy", 20);
        c.onEvent(TextTurnEvent::TurnDone, "", 25);
        c.setAnswerState(true, "answer");
        c.bindPages(1, 30);
        c.markPageRendered(31);
        check(c.contentChunks() == 1 && c.contentBytes() == 6,
              "content 分片计数与字节数");
        check(c.readyToFinish(9999) == false,
              "条件④: response_complete 缺失 → 不完成");
        c.onEvent(TextTurnEvent::ResponseComplete, "", 26);
        check(c.responseCompleteCount() == 1, "response_complete 计数");
        check(c.readyToFinish(9999), "补齐 response_complete 后可完成");
        c.onEvent(TextTurnEvent::ResponseComplete, "", 27);
        check(c.readyToFinish(9999) == false,
              "条件④b: response_complete 出现 2 次 → 不完成");

        TextTurnLifecycle d;
        d.configure(1000, 1000, 100, 100);
        d.setStartMs(0);
        d.onSubmitted("rid-d", 10);
        d.onEvent(TextTurnEvent::Content, "z", 20);
        d.onEvent(TextTurnEvent::ResponseComplete, "", 30);
        d.onEvent(TextTurnEvent::TurnDone, "", 40);
        d.setAnswerState(true, "z");
        d.bindPages(1, 50);
        d.markPageRendered(51);
        check(d.readyToFinish(9999) == false,
              "条件⑤: 无 expression 事件 → 不完成");
        d.onEvent(TextTurnEvent::Expression, "angry", 35);
        check(d.expressionCount() == 1 &&
              d.expressionValue() == "angry", "expression 值回读");
        check(d.readyToFinish(9999) == false,
              "条件⑤b: 非白名单情绪 → 不完成");
        d.onEvent(TextTurnEvent::Expression, "curious", 36);
        check(d.readyToFinish(9999) == false,
              "条件⑤c: expression 出现 2 次 → 不完成");

        TextTurnLifecycle e;
        e.configure(1000, 1000, 100, 100);
        e.setStartMs(0);
        e.onSubmitted("rid-e", 10);
        e.onEvent(TextTurnEvent::Content, "q", 20);
        e.onEvent(TextTurnEvent::ResponseComplete, "", 30);
        e.onEvent(TextTurnEvent::Expression, "happy", 35);
        e.setAnswerState(true, "q");
        e.bindPages(1, 50);
        e.markPageRendered(51);
        check(e.readyToFinish(9999) == false, "条件⑥: 无 done → 不完成");
        e.onEvent(TextTurnEvent::TurnDone, "", 60);
        check(e.doneCount() == 1, "done 计数");
        e.onEvent(TextTurnEvent::TurnDone, "", 61);
        check(e.readyToFinish(9999) == false, "条件⑥b: done 出现 2 次 → 不完成");
    }

    // ---- 4) 错误/取消: 终态码与"不得成功" ----
    {
        TextTurnLifecycle lc;
        lc.configure(1000, 1000, 100, 100);
        lc.setStartMs(0);
        lc.onSubmitted("rid-f", 10);
        lc.onEvent(TextTurnEvent::Content, "p", 20);
        lc.onEvent(TextTurnEvent::TurnError, "llm_http_401", 30);
        check(lc.errorCount() == 1 && lc.readyToFinish(99999) == false,
              "条件⑦: error 事件 → 不得成功");
        lc.markTerminalError();
        check(lc.exitCode() == TextTurnExit::TurnError, "Provider 错误 → 4");
        lc.finishSuccess();   // 迟到的强制结束也不得改写错误事实 (调用方约束)
        check(lc.exitCode() == TextTurnExit::Success,
              "显式 finishSuccess 是唯一成功写入口 (调用方需先过 readyToFinish)");

        TextTurnLifecycle cc;
        cc.configure(1000, 1000, 100, 100);
        cc.setStartMs(0);
        cc.onSubmitted("rid-g", 10);
        cc.onEvent(TextTurnEvent::TurnCancelled, "", 20);
        check(cc.cancelCount() == 1 && cc.readyToFinish(99999) == false,
              "条件⑧: cancel 事件 → 不得成功");
        cc.markCancelled();
        check(cc.exitCode() == TextTurnExit::Cancelled, "取消 → 5");

        TextTurnLifecycle ce;
        ce.configure(1000, 1000, 100, 100);
        ce.setStartMs(0);
        ce.onSubmitted("rid-h", 10);
        ce.onEvent(TextTurnEvent::TurnError, "boom", 20);
        ce.onEvent(TextTurnEvent::TurnCancelled, "", 21);
        check(ce.pickErrorExit() == TextTurnExit::Cancelled,
              "cancel 优先于 error (退出码 5 而非 4)");
        ce.markTerminalError();
        check(ce.exitCode() == TextTurnExit::Cancelled,
              "混合终态: markTerminalError 取 cancel 优先");

        TextTurnLifecycle wl;
        wl.configure(1000, 1000, 100, 100);
        wl.setStartMs(0);
        wl.onSubmitted("rid-i", 10);
        wl.onEvent(TextTurnEvent::WorkerLost, "", 20);
        check(wl.readyToFinish(99999) == false,
              "worker 掉线 → 不得成功");
    }

    // ---- 5) 分页逐页展示: 顺序/完整性/末页 hold ----
    {
        Turn t(4);
        check(t.lc.pageCount() == 4, "页数回读 = 4");
        check(t.lc.pagesShown().size() == 1 &&
              t.lc.pagesShown()[0].index == 0, "首屏记录 index=0");
        check(t.lc.phase() == TextTurnPhase::HoldingPages,
              "绑定页数后阶段 = HoldingPages");
        check(t.lc.pollTimeout(999999) == -1,
              "展示阶段不计入 provider/终态超时");
        check(t.lc.shouldAdvancePage(61) == false, "dwell 未满不翻页");
        check(t.lc.shouldAdvancePage(62 + 1000), "dwell 满 → 允许翻页");
        t.lc.advancePage(1062);
        check(t.lc.pagesShown().size() == 2 &&
              t.lc.pagesShown().back().index == 1, "翻页记录 index=1");
        check(t.lc.pagesShown().back().rendered == false,
              "新页在渲染前 rendered=false");
        check(t.lc.readyToFinish(999999) == false,
              "存在未渲染页 → 不得完成");
        t.lc.markPageRendered(1063);
        check(t.lc.pagesShown()[1].rendered_first_ms == 1063,
              "每页记录首次渲染时刻");
        t.lc.advancePage(2063);
        t.lc.markPageRendered(2064);
        t.lc.advancePage(3064);
        check(t.lc.pagesShown().size() == 4, "第 4 页记录存在");
        t.lc.markPageRendered(3065);
        check(t.lc.shouldAdvancePage(999999) == false, "末页不再翻页");
        check(t.lc.finalHoldSatisfied(3065 + 799) == false,
              "末页 hold 未满");
        t.lc.finalHoldSatisfied(3065 + 800);
        check(t.lc.finalHoldSatisfied(3065 + 800), "末页 hold 到点");
        // 顺序连续且全部渲染 → 允许完成
        t.lc.markPageRendered(3066);
        check(t.lc.readyToFinish(3065 + 800) == true,
              "四页顺序展示 + 末页 hold → 可完成");

        Turn one(1);
        check(one.lc.shouldAdvancePage(999999) == false,
              "单页: 永不翻页");
        one.lc.markPageRendered(100);
        check(one.lc.finalHoldSatisfied(62 + 800), "单页 hold 以首次渲染计时");
        check(one.lc.readyToFinish(62 + 800), "单页完整展示 → 可完成");

        Turn zero(0);
        check(zero.lc.pageCount() == 0 && zero.lc.pagesShown().empty(),
              "bindPages(0) 不产生页面记录");
        check(zero.lc.readyToFinish(999999) == false,
              "条件⑩: 无页面展示 → 不得完成 (exit 7 路径)");
        zero.lc.finishWith(TextTurnExit::PageOrAuditFail);
        check(zero.lc.exitCode() == TextTurnExit::PageOrAuditFail,
              "页面/审计失败 → 7");
    }

    // ---- 5b) R8_R2_R2 (P1-5): 每页停留时长证据 ----
    {
        Turn t(3, 1000, 800);
        check(t.lc.pageDwellMs(0, 62 + 5000) == 5000,
              "未翻页时该页即末页: 停留随结束时刻增长");
        t.lc.advancePage(62 + 1000);
        check(t.lc.pagesShown()[0].advance_ms == 62 + 1000,
              "翻页记录离开时刻 advance_ms");
        t.lc.markPageRendered(62 + 1001);
        check(t.lc.pageDwellMs(0, 999999) == 1000,
              "非末页停留 = 翻页时刻 - 首次渲染时刻");
        check(t.lc.pageDwellMs(1, 62 + 1001 + 800) == 800,
              "末页停留 = 结束时刻 - 首次渲染时刻");
        check(t.lc.pageDwellMs(1, 62 + 1001 + 799) == 799,
              "末页停留按结束时刻精确计算");
        check(t.lc.pageDwellMs(9, 999999) == 0, "越界页停留为 0");
        check(t.lc.pageDwellMs(1, 0) == 0, "结束时刻早于渲染 → 0 (不误报)");
    }

    // ---- 6) 终态后迟到事件一律忽略 ----
    {
        Turn t;
        t.lc.finishSuccess();
        const int chunks = t.lc.contentChunks();
        const int exprs = t.lc.expressionCount();
        t.lc.onEvent(TextTurnEvent::Content, "late", 5000);
        t.lc.onEvent(TextTurnEvent::Expression, "sad", 5000);
        t.lc.onEvent(TextTurnEvent::TurnError, "late-err", 5000);
        check(t.lc.contentChunks() == chunks && t.lc.expressionCount() == exprs,
              "成功后迟到事件不改变计数");
        check(t.lc.errorCount() == 0, "成功后迟到错误不改写为失败");
        check(t.lc.exitCode() == TextTurnExit::Success, "成功码保持 0");

        Turn e2(4);
        e2.lc.markTerminalError();
        const size_t pages = e2.lc.pagesShown().size();
        e2.lc.onEvent(TextTurnEvent::TurnDone, "", 5000);
        e2.lc.advancePage(6000);
        check(e2.lc.pagesShown().size() == pages,
              "失败终态后不再产生页面记录");
    }

    // ---- 7) SHA-256: 标准向量 ----
    {
        check(sha256Hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA-256 空串向量");
        check(sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256 'abc' 向量");
        check(sha256Hex("hello world") ==
              "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
              "SHA-256 'hello world' 向量");
        check(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
              == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "SHA-256 56 字节向量 (跨填充边界)");
        TextTurnLifecycle lc;
        lc.setAnswerState(true, "回答内容");
        check(lc.committedBytes() == std::string("回答内容").size() &&
              lc.committedSha256() == sha256Hex("回答内容"),
              "committed answer 的字节数与摘要一致");
        lc.setAnswerState(false, "");
        check(lc.committedBytes() == 0 && lc.committedSha256() == sha256Hex(""),
              "空回答: 字节 0 且摘要为合法空串摘要");
    }

    // ---- 8) 平台位图 (避免 -Wunused 时的静态检查依赖) + 白名单情绪 ----
    {
        check(isWhitelistedEmotion("neutral") && isWhitelistedEmotion("happy") &&
              isWhitelistedEmotion("curious") && isWhitelistedEmotion("surprised") &&
              isWhitelistedEmotion("sad") && isWhitelistedEmotion("sleepy") &&
              isWhitelistedEmotion("concerned"),
              "白名单情绪 7 项全部接受");
        check(!isWhitelistedEmotion("angry") && !isWhitelistedEmotion("") &&
              !isWhitelistedEmotion("HAPPY"),
              "白名单外/空/大小写变体一律拒绝");
    }

    // ---- 9) 输入校验纯函数 (--text-once-file) ----
    {
        const fs::path dir = fs::temp_directory_path() /
            ("holopet_lifecycle_" + std::to_string(
                static_cast<unsigned long long>(::time(nullptr))));
        fs::create_directories(dir);
        auto put = [&](const std::string& name, const std::string& body) {
            std::ofstream f(dir / name, std::ios::binary);
            f << body;
            return (dir / name).string();
        };
        std::string err;

        auto ok = loadTextOnceFile(put("ok.txt", "  \n你好 HoloPet\n\n"), err);
        check(ok.has_value() && *ok == "你好 HoloPet",
              "输入校验: 首尾空白剥离, 内部内容保留");
        check(err.empty(), "输入校验: 成功时 err 为空");

        check(!loadTextOnceFile((dir / "missing.txt").string(), err)
                   .has_value() && err == "missing_file",
              "输入校验: 缺文件 → missing_file");
        check(!loadTextOnceFile(put("empty.txt", ""), err).has_value() &&
              err == "empty_file", "输入校验: 空文件 → empty_file");
        check(!loadTextOnceFile(put("blank.txt", " \r\n\t "), err).has_value() &&
              err == "empty_file", "输入校验: 纯空白 → empty_file");
        check(!loadTextOnceFile(put("big.txt", std::string(8193, 'a')), err)
                   .has_value() && err == "too_large",
              "输入校验: 超 8 KiB → too_large");
        std::string bad = "ok";
        bad.push_back(static_cast<char>(0xFF));
        bad.push_back(static_cast<char>(0xFE));
        check(!loadTextOnceFile(put("bad.txt", bad), err).has_value() &&
              err == "invalid_utf8", "输入校验: 无效 UTF-8 → invalid_utf8");
        check(loadTextOnceFile(put("edge.txt", std::string(8192, 'a')), err)
                   .has_value(), "输入校验: 恰好 8 KiB 接受");

        fs::remove_all(dir);
        check(!fs::exists(dir), "输入校验临时目录已清理");
    }

    // ---- 10) 事件审计: 原子写入 + fail closed ----
    {
        const fs::path dir = fs::temp_directory_path() /
            ("holopet_audit_" + std::to_string(
                static_cast<unsigned long long>(::time(nullptr))));
        fs::create_directories(dir);
        const std::string apath = (dir / "text_events.jsonl").string();

        TextEventAudit audit;
        check(audit.failed() == false, "审计: 初始未失败");
        check(audit.open(apath), "审计: 在可写目录 open 成功");
        check(fs::exists(apath + ".tmp"), "审计: 写入期间只有 .tmp 文件");
        check(!fs::exists(apath), "审计: 提交前正式文件不存在 (原子)");
        check(audit.writeLine("{\"event\":\"start\",\"run_id\":\"r8_r2_r1\"}"),
              "审计: 写入 start 行");
        check(audit.writeLine("{\"event\":\"page_shown\",\"index\":0}"),
              "审计: 写入 page_shown 行");
        check(audit.commit(), "审计: commit 成功");
        check(fs::exists(apath) && !fs::exists(apath + ".tmp"),
              "审计: commit 后 .tmp 消失, 正式文件出现");
        check(!audit.failed(), "审计: commit 后未标记失败");
        {
            std::ifstream rd(apath, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(rd)),
                             std::istreambuf_iterator<char>());
            const std::string want =
                "{\"event\":\"start\",\"run_id\":\"r8_r2_r1\"}\n"
                "{\"event\":\"page_shown\",\"index\":0}\n";
            check(body == want, "审计: 逐行内容与顺序完全一致 (JSONL)");
        }

        TextEventAudit bad;
        const std::string nopath = (dir / "nodir" / "x.jsonl").string();
        check(bad.open(nopath) == false && bad.failed(),
              "审计: 不可写路径 → open 失败且标记 failed (exit 7 依据)");
        check(bad.commit() == false, "审计: 未打开时 commit 失败");
        check(!fs::exists(nopath), "审计: 失败路径不产生正式文件");

        TextEventAudit fail2;
        check(fail2.open(apath + ".2"), "审计: 第二个审计文件 open 成功");
        check(fail2.writeLine("{}"), "审计: 第二文件写入一行");
        check(fail2.commit(), "审计: 第二文件 commit 成功");

        fs::remove_all(dir);
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
