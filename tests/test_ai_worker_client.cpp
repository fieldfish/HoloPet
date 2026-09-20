/**
 * test_ai_worker_client.cpp — V6 行缓冲/request_id 过滤/事件映射测试
 */

#include "ipc/ai_worker_client.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_ai_worker_client (V6) ===\n\n";

    // LineBuffer: 部分读取
    {
        LineBuffer lb;
        check(!lb.popLine().has_value(), "空缓冲无行");
        lb.append("{\"a\":1}", 7);
        check(!lb.popLine().has_value(), "无换行不出行");
        lb.append("\n{\"b\":2}\n", 9);
        auto l1 = lb.popLine();
        check(l1 && *l1 == "{\"a\":1}", "部分读取拼接完整行");
        auto l2 = lb.popLine();
        check(l2 && *l2 == "{\"b\":2}", "第二行");
        check(!lb.popLine().has_value(), "缓冲排空");

        // CRLF
        lb.append("{\"c\":3}\r\n", 9);
        auto l3 = lb.popLine();
        check(l3 && *l3 == "{\"c\":3}", "CRLF 剥离 \\r");
    }

    // RequestIdFilter: 过期 id 丢弃
    {
        RequestIdFilter f;
        WorkerMessage m;
        m.request_id = "old";
        m.type = "transcript";
        check(!f.accept(m), "无活动轮次丢弃");

        f.setActive("current");
        m.request_id = "old";
        check(!f.accept(m), "过期 request_id 丢弃");

        m.request_id = "current";
        check(f.accept(m), "活动 request_id 接受");

        m.type = "hello"; m.request_id = "whatever";
        check(f.accept(m), "hello 不参与过滤");

        // R2: expression 与 transcript/state/tts/done 同等过滤 (旧轮表情不污染新轮)
        m.type = "expression"; m.request_id = "old";
        check(!f.accept(m), "旧轮 expression 丢弃");
        m.request_id = "current";
        check(f.accept(m), "活动轮 expression 接受");

        f.clear();
        m.type = "done"; m.request_id = "current";
        check(!f.accept(m), "clear 后丢弃");
    }

    // WorkerMessage → AgentEvent 映射 (R4 合同: v + request_id 必填)
    {
        auto r1 = parseWorkerLine(
            R"({"v":"1","request_id":"r1","type":"state","value":"thinking"})");
        auto ev = toAgentEvent(*r1.message);
        check(ev.has_value() && ev->type == AgentEventType::StateChanged &&
              ev->state == RuntimeState::Thinking, "state→StateChanged(Thinking)");

        auto r2 = parseWorkerLine(
            R"({"v":"1","request_id":"r2","type":"transcript","text":"你好"})");
        ev = toAgentEvent(*r2.message);
        check(ev && ev->type == AgentEventType::Transcript && ev->text == "你好",
              "transcript→Transcript");

        auto r3 = parseWorkerLine(
            R"({"v":"1","request_id":"r3","type":"expression","value":"sad"})");
        ev = toAgentEvent(*r3.message);
        check(ev && ev->type == AgentEventType::Expression &&
              ev->emotion == Emotion::Sad, "expression→Sad");

        auto r4 = parseWorkerLine(
            R"({"v":"1","request_id":"r4","type":"tts_level","rms":0.7})");
        ev = toAgentEvent(*r4.message);
        check(ev && ev->type == AgentEventType::TtsLevel && ev->level > 0.69,
              "tts_level→TtsLevel");

        // R4: 未知 type 在 parse 层即结构化错误 (不再产出 message)
        auto r5 = parseWorkerLine(
            R"({"v":"1","request_id":"r5","type":"mystery"})");
        check(!r5.message && r5.error && r5.error->reason == "unknown_type",
              "未知 type → parse 层 unknown_type");

        auto r6 = parseWorkerLine(
            R"({"v":"1","request_id":"r6","type":"error","text":"bad"})");
        ev = toAgentEvent(*r6.message);
        check(ev && ev->type == AgentEventType::TurnError && ev->text == "bad",
              "error→TurnError");
    }

    // WorkerEventQueue 有界
    {
        WorkerEventQueue q;
        for (int i = 0; i < 600; ++i) {
            AgentEvent e; e.type = AgentEventType::TtsLevel; e.level = i * 0.001;
            q.push(e);
        }
        AgentEvent e;
        int n = 0;
        while (q.tryPop(e)) ++n;
        check(n == 512, "队列上限 512 防爆");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    // ---- R8_R2_R6 (A): 错误身份 = 协议稳定 code, text 仅作说明 ----
    {
        auto m1 = parseWorkerLine(
            R"({"v":"1","request_id":"rA","type":"error","code":"llm_missing_key","text":"missing_api_key"})");
        check(m1.message.has_value() && m1.message->code == "llm_missing_key",
              "R6A-1 解析 code 字段");
        auto ev1 = toAgentEvent(*m1.message);
        check(ev1 && ev1->type == AgentEventType::TurnError
                  && ev1->error_code == "llm_missing_key",
              "R6A-1 公开错误码 = llm_missing_key (不得是 text)");
        check(ev1 && ev1->text == "missing_api_key",
              "R6A-1 text 保留为内部说明");

        auto m2 = parseWorkerLine(
            R"({"v":"1","request_id":"rB","type":"error","code":"llm_timeout","text":"timeout"})");
        auto ev2 = toAgentEvent(*m2.message);
        check(ev2 && ev2->error_code == "llm_timeout",
              "R6A-2 公开错误码 = llm_timeout (不得是 timeout)");
        check(ev2 && ev2->text == "timeout", "R6A-2 text 保留");

        auto m3 = parseWorkerLine(
            R"({"v":"1","request_id":"rC","type":"error","code":"llm_network_error","text":"some-internal-detail"})");
        auto ev3 = toAgentEvent(*m3.message);
        check(ev3 && ev3->error_code == "llm_network_error"
                  && ev3->text == "some-internal-detail",
              "R6A-3 code!=text 时公开码以 code 为准");

        auto m4 = parseWorkerLine(
            R"({"v":"1","request_id":"rD","type":"error","text":"legacy"})");
        auto ev4 = toAgentEvent(*m4.message);
        check(ev4 && ev4->error_code == "legacy",
              "R6A-4 无 code 的历史消息安全回退 text");

        auto c1 = toAgentEvent(*parseWorkerLine(
            R"({"v":"1","request_id":"rE","type":"content","text":"你好"})").message);
        auto c2 = toAgentEvent(*parseWorkerLine(
            R"({"v":"1","request_id":"rE","type":"response_complete"})").message);
        auto c3 = toAgentEvent(*parseWorkerLine(
            R"({"v":"1","request_id":"rE","type":"expression","value":"happy"})").message);
        auto c4 = toAgentEvent(*parseWorkerLine(
            R"({"v":"1","request_id":"rE","type":"done"})").message);
        check(c1 && c1->type == AgentEventType::ContentChunk && c1->text == "你好"
                  && c1->error_code.empty(),
              "R6A-5 content 事件不变且不带 error_code");
        check(c2 && c2->type == AgentEventType::ResponseComplete
                  && c3 && c3->type == AgentEventType::Expression
                  && c3->emotion == Emotion::Happy
                  && c4 && c4->type == AgentEventType::TurnDone,
              "R6A-5 response_complete/expression/done 不回归");
    }

    return g_failed == 0 ? 0 : 1;
}
