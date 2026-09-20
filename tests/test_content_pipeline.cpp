/**
 * test_content_pipeline.cpp — R4-R1 P1 (D1/D10): 回答正文进入产品链
 *
 * 断言:
 *  - 中文与转义 content 分片拼接完全一致
 *  - response_complete 后 UI snapshot 使用 answer 而非 transcript
 *  - 空回答有且只有一种稳定终态 (response_complete + done)
 *  - 累积字节上限生效 (超限丢弃后续分片, 不崩溃)
 *  - 新轮/cancel/error 的清理规则
 */

#include "agent/agent_event.hpp"
#include "agent/conversation_controller.hpp"
#include "ipc/ai_worker_client.hpp"     // toAgentEvent
#include "ipc/ai_worker_protocol.hpp"  // parseWorkerLine

#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_content_pipeline (V6 R4-R1) ===\n\n";

    // ---- 1. 中文与转义分片经真实映射 → 事件 → 聚合 ----
    {
        const std::string chunk1 = "你好，\"世界\"\n第二行";
        const std::string chunk2 = "——结尾。";
        ConversationController c;

        auto feed = [&](const std::string& text) {
            auto m = parseWorkerLine(
                "{\"v\":\"1\",\"request_id\":\"r\",\"type\":\"content\",\"text\":" +
                jsonEscape(text) + "}");
            auto ev = toAgentEvent(*m.message);
            return c.handleAgent(*ev);
        };
        AgentEvent started; started.type = AgentEventType::TurnStarted;
        c.handleAgent(started);
        feed(chunk1);
        feed(chunk2);
        check(c.pendingAnswer() == chunk1 + chunk2, "content 逐块追加 (含中文/转义)");
        check(!c.answerValid(), "response_complete 前未提交");

        auto rp = parseWorkerLine(
            R"({"v":"1","request_id":"r","type":"response_complete"})");
        c.handleAgent(*toAgentEvent(*rp.message));
        check(c.answerValid(), "response_complete 后提交");
        check(c.committedAnswer() == chunk1 + chunk2, "提交内容 = 全部 content 拼接");

        // UI snapshot 用 answer 而非 transcript
        AgentEvent tr; tr.type = AgentEventType::Transcript; tr.text = "用户的原始话";
        c.handleAgent(tr);
        check(c.displayText() == chunk1 + chunk2, "UI 显示 answer 而非 transcript");
        check(c.userTranscript() == "用户的原始话", "transcript 独立保存");
    }

    // ---- 2. 空回答唯一稳定终态: response_complete + done ----
    {
        ConversationController c;
        AgentEvent started; started.type = AgentEventType::TurnStarted;
        c.handleAgent(started);
        AgentEvent st; st.type = AgentEventType::StateChanged;
        st.state = RuntimeState::Thinking;
        c.handleAgent(st, 0);
        auto rp = parseWorkerLine(
            R"({"v":"1","request_id":"r","type":"response_complete"})");
        c.handleAgent(*toAgentEvent(*rp.message));
        AgentEvent done; done.type = AgentEventType::TurnDone;
        c.handleAgent(done);
        check(c.answerValid(), "无 content 时 response_complete 也是有效终态");
        check(c.committedAnswer().empty(), "空回答为空串");
        check(c.state() == RuntimeState::Idle, "done 后回 Idle");
    }

    // ---- 3. 字节上限 ----
    {
        ConversationController c;
        AgentEvent started; started.type = AgentEventType::TurnStarted;
        c.handleAgent(started);
        AgentEvent ch; ch.type = AgentEventType::ContentChunk;
        ch.text = std::string(ConversationController::kMaxAnswerBytes - 10, 'a');
        c.handleAgent(ch);
        ch.text = "012345678901234567890";   // 超限
        c.handleAgent(ch);
        check(c.pendingAnswer().size() == ConversationController::kMaxAnswerBytes - 10,
              "超出 kMaxAnswerBytes 的分片被丢弃");
    }

    // ---- 4. 清理规则: 新轮复位 / cancel 丢 pending 留 committed ----
    {
        ConversationController c;
        AgentEvent started; started.type = AgentEventType::TurnStarted;
        c.handleAgent(started);
        AgentEvent ch; ch.type = AgentEventType::ContentChunk; ch.text = "A";
        c.handleAgent(ch);
        auto rp = parseWorkerLine(
            R"({"v":"1","request_id":"r","type":"response_complete"})");
        c.handleAgent(*toAgentEvent(*rp.message));
        AgentEvent ch2; ch2.type = AgentEventType::ContentChunk; ch2.text = "B";
        c.handleAgent(ch2);                     // 新 pending
        AgentEvent canc; canc.type = AgentEventType::TurnCancelled;
        c.handleAgent(canc);
        check(c.pendingAnswer().empty(), "cancel 丢弃未提交回答");
        check(c.committedAnswer() == "A", "cancel 保留已提交回答");
        c.handleAgent(started);                 // 新轮
        check(!c.answerValid() && c.committedAnswer().empty() &&
              c.pendingAnswer().empty(), "新轮三文本复位");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
