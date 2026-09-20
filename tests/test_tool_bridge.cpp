// test_tool_bridge.cpp — R8_R4 §10.2/§5 工具协议桥确定性测试。
#include <iostream>
#include <string>

#include "features/tool_bridge.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_tool_bridge (R8_R4) ===\n\n";

    LocalFeatureService svc;
    ToolBridge bridge(svc);

    {
        // 1. 解析严格性: v 必须为 1; 结构损坏拒绝
        auto ok = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r1\",\"tool_call_id\":\"c1\","
            "\"name\":\"create_timer\",\"arguments\":{\"duration_ms\":\"60000\"}}");
        check(ok && ok->name == "create_timer"
              && ok->args["duration_ms"] == "60000", "合法请求解析成功");
        // R8_R4_R2: agentd 线上字符串形态 arguments (内层为 JSON 文本) —
        // Pi 实测 tool_timeout 根因回归
        auto str_ok = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r1b\",\"tool_call_id\":\"c1b\","
            "\"name\":\"add_note\",\"arguments\":\"{\\\"text\\\": "
            "\\\"测试便签内容\\\"}\"}");
        check(str_ok && str_ok->name == "add_note"
              && str_ok->args["text"] == "测试便签内容",
              "字符串形态 arguments (转义引号) 解析成功");
        auto str_esc = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r1c\",\"tool_call_id\":\"c1c\","
            "\"name\":\"add_note\",\"arguments\":\"{\\\"text\\\": "
            "\\\"路径 C:\\\\\\\\dir\\\\\\\\file\\\"}\"}");
        check(str_esc && str_esc->args["text"] == "路径 C:\\dir\\file",
              "字符串形态 arguments 转义反斜杠解出正确");
        auto str_num = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r1f\",\"tool_call_id\":\"c1f\","
            "\"name\":\"create_timer\",\"arguments\":\"{\\\"duration_ms\\\": "
            "30000}\"}");
        check(str_num && str_num->args["duration_ms"] == "30000",
              "字符串形态 arguments 数字参数归一化为字符串");
        check(!parseToolRequest(
                  "{\"v\":\"1\",\"request_id\":\"r1d\",\"tool_call_id\":\"c1d\","
                  "\"name\":\"add_note\",\"arguments\":\"not json\"}"),
              "字符串形态 arguments 非 JSON 拒绝");
        check(!parseToolRequest(
                  "{\"v\":\"1\",\"request_id\":\"r1e\",\"tool_call_id\":\"c1e\","
                  "\"name\":\"add_note\",\"arguments\":\"{\\\"text\\\": "
                  "\\\"abc\"}"),
              "字符串形态 arguments 内层未闭合拒绝");
        check(!parseToolRequest(
                  "{\"v\":\"2\",\"request_id\":\"r\",\"tool_call_id\":\"c\","
                  "\"name\":\"x\"}"), "v≠1 拒绝");
        check(!parseToolRequest(
                  "{\"v\":\"1\",\"request_id\":\"r\",\"tool_call_id\":\"c\","
                  "\"name\":\"x\", broken"), "损坏 JSON 拒绝");
        check(!parseToolRequest("{\"v\":\"1\"}"), "缺字段拒绝");
    }

    {
        // 2. 幂等: 同一 (request_id, tool_call_id) 二次 → 无副作用 + 缓存结果
        auto req = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r2\",\"tool_call_id\":\"c2\","
            "\"name\":\"create_timer\",\"arguments\":{\"duration_ms\":\"30000\","
            "\"label\":\"tea\"}}");
        auto r1 = bridge.handle(*req, 0, 0);
        check(r1.ok && !r1.result["id"].empty(), "首次创建成功");
        check(bridge.featureEvent().emitted
              && bridge.featureEvent().feature == "timer", "产生 feature_event");
        auto r2 = bridge.handle(*req, 1000, 1000);
        check(r2.ok && r2.code == "duplicate_ignored"
              && r2.result["id"] == r1.result["id"], "重复请求无副作用");
        check(svc.listTimers().size() == 1, "定时器仍只有 1 个");
    }

    {
        // 3. 未知工具/缺参数/越界 → 稳定失败
        auto u = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r\",\"tool_call_id\":\"c\","
            "\"name\":\"sudo_rm\",\"arguments\":{}}");
        check(!bridge.handle(*u, 0, 0).ok
              && bridge.handle(*u, 0, 0).code == "duplicate_ignored",
              "未知工具 → unknown_tool (且幂等缓存)");
        auto m = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r3\",\"tool_call_id\":\"c3\","
            "\"name\":\"create_timer\",\"arguments\":{}}");
        auto rm = bridge.handle(*m, 0, 0);
        check(!rm.ok && rm.code == "invalid_arg", "缺 duration_ms → invalid_arg");
        auto o = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r4\",\"tool_call_id\":\"c4\","
            "\"name\":\"create_timer\",\"arguments\":{\"duration_ms\":\"-5\"}}");
        auto ro = bridge.handle(*o, 0, 0);
        check(!ro.ok && ro.code == "invalid_arg", "越界时长 → invalid_arg");
    }

    {
        // 4. 危险模糊命令 → confirm_required (删除全部/清空)
        auto d = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r5\",\"tool_call_id\":\"c5\","
            "\"name\":\"delete_note\",\"arguments\":{\"id\":\"all\"}}");
        auto rd = bridge.handle(*d, 0, 0);
        check(!rd.ok && rd.code == "confirm_required",
              "清空全部 → confirm_required");
        auto t = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r6\",\"tool_call_id\":\"c6\","
            "\"name\":\"cancel_timer\",\"arguments\":{\"id\":\"all\"}}");
        auto rt = bridge.handle(*t, 0, 0);
        check(!rt.ok && rt.code == "confirm_required",
              "取消全部定时器 → confirm_required");
    }

    {
        // 5. UI 与语音共用同一服务: 两个桥写同一状态
        ToolBridge bridge2(svc);
        auto r1 = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"u1\",\"tool_call_id\":\"u1\","
            "\"name\":\"add_note\",\"arguments\":{\"text\":\"明天拿快递\"}}");
        check(bridge.handle(*r1, 0, 1000).ok, "桥1 加便签");
        auto r2 = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"u2\",\"tool_call_id\":\"u2\","
            "\"name\":\"list_notes\",\"arguments\":{\"limit\":\"5\"}}");
        auto l2 = bridge2.handle(*r2, 0, 2000);
        check(l2.ok && l2.result["count"] == "1", "桥2 读到桥1 写入的便签 (同一状态)");
    }

    {
        // 6. 便签文本含转义/中文往返; 空文本拒绝
        auto n = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r7\",\"tool_call_id\":\"c7\","
            "\"name\":\"add_note\",\"arguments\":{\"text\":\"你好 \\\"世界\\\" ☕\"}}");
        auto rn = bridge.handle(*n, 0, 0);
        check(rn.ok, "转义中文便签创建");
        auto rd = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r8\",\"tool_call_id\":\"c8\","
            "\"name\":\"read_note\",\"arguments\":{\"id\":\"" + rn.result["id"] + "\"}}");
        auto rr = bridge.handle(*rd, 0, 0);
        check(rr.ok && rr.result["text"] == "你好 \"世界\" ☕", "读取原文一致");
        auto ne = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"r9\",\"tool_call_id\":\"c9\","
            "\"name\":\"add_note\",\"arguments\":{\"text\":\"\"}}");
        auto rne = bridge.handle(*ne, 0, 0);
        check(!rne.ok && rne.code == "invalid_arg", "空便签 → invalid_arg");
    }

    {
        // 7. R5_R4 (B2): get_ai_mode / set_ai_mode / dismiss_alert
        auto g = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"m1\",\"tool_call_id\":\"m1\","
            "\"name\":\"get_ai_mode\",\"arguments\":{}}");
        auto rg = bridge.handle(*g, 0, 0);
        check(rg.ok && rg.result["mode"] == "auto", "get_ai_mode 默认 auto");
        auto s = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"m2\",\"tool_call_id\":\"m2\","
            "\"name\":\"set_ai_mode\",\"arguments\":{\"mode\":\"fast\"}}");
        auto rs = bridge.handle(*s, 0, 0);
        check(rs.ok && rs.result["mode"] == "fast", "set_ai_mode fast");
        auto sb = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"m3\",\"tool_call_id\":\"m3\","
            "\"name\":\"set_ai_mode\",\"arguments\":{\"mode\":\"hack\"}}");
        auto rsb = bridge.handle(*sb, 0, 0);
        check(!rsb.ok && rsb.code == "invalid_arg",
              "set_ai_mode 非法枚举 → invalid_arg");
        auto d = parseToolRequest(
            "{\"v\":\"1\",\"request_id\":\"m4\",\"tool_call_id\":\"m4\","
            "\"name\":\"dismiss_alert\",\"arguments\":{}}");
        auto rd = bridge.handle(*d, 0, 0);
        check(rd.ok, "dismiss_alert ok");
        check(svc.consumeAlertDismiss(), "响铃停止信号一次性消费");
        check(!svc.consumeAlertDismiss(), "信号消费后复位");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
