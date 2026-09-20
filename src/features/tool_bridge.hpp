#pragma once
/**
 * tool_bridge.hpp — R8_R4 受限工具协议桥 (纯 C++, 无 SDL)。
 *
 * 版本化 UDS 工具请求 → 统一 LocalFeatureService → 结构化结果 (§5)。
 * 协议:
 *   tool_request  {v:"1", request_id, tool_call_id, name, arguments:{...}}
 *   tool_result   {v:"1", request_id, tool_call_id, ok, code, result:{...}}
 *   feature_event {v:"1", feature, action, item_id, monotonic_ms}
 *
 * 幂等: (request_id, tool_call_id) 已见 → 返回缓存结果, 绝不重复副作用。
 * 稳定错误: unknown_tool | invalid_arg | not_found | limit_exceeded |
 *           confirm_required (删除全部/清空需二次确认) | duplicate_ignored。
 */
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "features/local_feature_service.hpp"
#include "system/mini_json.hpp"

namespace holopet {

struct ToolRequest {
    std::string request_id;
    std::string tool_call_id;
    std::string name;
    std::map<std::string, std::string> args;   // 结构化字符串参数 (严格校验后取用)
};

struct ToolResult {
    bool ok = true;
    std::string code;
    std::map<std::string, std::string> result;
};

struct FeatureEventOut {
    bool emitted = false;
    std::string feature;
    std::string action;
    std::string item_id;
    int64_t monotonic_ms = 0;
};

/** R8_R4_R2: 工具请求解析 — 真实 JSON 解析器 (system/mini_json.hpp)。
 * 旧手工逐键扫描器无法正确处理转义 (Pi 实测 tool_timeout 根因:
 * 带参数的 tool_request 全部被拒收)。ev.text 本身是合法 JSON
 * (agentd 发的是 req 的 JSON 编码), 全量解析最稳:
 *   arguments 为字符串形态 ("{\"key\":\"value\"}") 或对象形态均可;
 *   值类型字符串/数字/布尔统一归一化为字符串参数表。
 */
inline std::optional<ToolRequest> parseToolRequest(const std::string& json) {
    ProtocolError jerr;
    auto doc = parseJson(json, &jerr);
    if (!doc || doc->type != JsonValue::Type::Object) return std::nullopt;
    auto v = doc->getString("v");
    auto rid = doc->getString("request_id");
    auto cid = doc->getString("tool_call_id");
    auto nm = doc->getString("name");
    if (!v || *v != "1" || !rid || !cid || !nm) return std::nullopt;
    ToolRequest r;
    r.request_id = *rid;
    r.tool_call_id = *cid;
    r.name = *nm;
    // 参数表填充 (值类型归一化为字符串; 嵌套结构拒绝)
    auto collect = [&](const JsonValue& obj) -> bool {
        for (const auto& kv : obj.obj) {
            const JsonValue& val = kv.second;
            if (val.type == JsonValue::Type::String) {
                r.args[kv.first] = val.str;
            } else if (val.type == JsonValue::Type::Number) {
                // 数字参数 (模型常发): 整数直转, 小数保精度
                const double d = val.num;
                if (d == static_cast<double>(static_cast<int64_t>(d)))
                    r.args[kv.first] =
                        std::to_string(static_cast<int64_t>(d));
                else
                    r.args[kv.first] = std::to_string(d);
            } else if (val.type == JsonValue::Type::Bool) {
                r.args[kv.first] = val.b ? "true" : "false";
            } else {
                return false;   // null/array/object 嵌套 → 拒绝
            }
        }
        return true;
    };
    if (const JsonValue* sv = doc->get("arguments")) {
        if (sv->type == JsonValue::Type::Object) {
            if (!collect(*sv)) return std::nullopt;   // 对象形态 (测试/兼容)
        } else if (sv->type == JsonValue::Type::String) {
            // 字符串形态: 内层仍是 JSON 文本, 再解析一次
            auto inner = parseJson(sv->str, &jerr);
            if (!inner || inner->type != JsonValue::Type::Object)
                return std::nullopt;
            if (!collect(*inner)) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return r;
}

inline std::string dumpToolResult(const std::string& request_id,
                                  const std::string& tool_call_id,
                                  const ToolResult& r) {
    std::string s = "{\"type\":\"tool_result\",\"v\":\"1\","
        "\"request_id\":\"" + request_id
        + "\",\"tool_call_id\":\"" + tool_call_id + "\",\"ok\":"
        + (r.ok ? "true" : "false") + ",\"code\":\"" + r.code
        + "\",\"result\":{";
    bool first = true;
    for (const auto& kv : r.result) {
        if (!first) s += ",";
        first = false;
        // R8_R4_R2: 值先完整转义进局部再拼 "key":"value" —
        // 旧实现把转义片段直接写进 s 又追加原值, 产出坏 JSON
        // (Pi 实测 tool_timeout 根因之一: tool_result 行解析失败)。
        std::string v;
        for (size_t i = 0; i < kv.second.size(); ++i) {
            char c = kv.second[i];
            if (c == '"' || c == '\\') { v += '\\'; v += c; }
            else if (c == '\n') v += "\\n";
            else v += c;
        }
        s += "\"" + kv.first + "\":\"" + v + "\"";
    }
    s += "}}";
    return s;
}

class ToolBridge {
public:
    explicit ToolBridge(LocalFeatureService& svc) : svc_(svc) {}

    /** 处理一条 tool_request (已解析); 幂等由 seen_ 保证。 */
    ToolResult handle(const ToolRequest& req, int64_t now_mono_ms,
                      int64_t now_wall_ms) {
        feature_out_ = FeatureEventOut{};
        std::string key = req.request_id + "/" + req.tool_call_id;
        auto it = seen_.find(key);
        if (it != seen_.end()) {
            ToolResult r;
            r.ok = true;
            r.code = "duplicate_ignored";
            r.result = it->second;
            return r;
        }
        ToolResult r = dispatch(req, now_mono_ms, now_wall_ms);
        seen_[key] = r.result;
        return r;
    }

    const FeatureEventOut& featureEvent() const { return feature_out_; }

private:
    FeatureError fail(ToolResult& r, const std::string& code) {
        r.ok = false;
        r.code = code;
        return FeatureError{false, code};
    }

    ToolResult dispatch(const ToolRequest& req, int64_t now_mono_ms,
                        int64_t now_wall_ms) {
        ToolResult r;
        const auto& a = req.args;
        if (req.name == "create_timer") {
            auto d = a.find("duration_ms");
            if (d == a.end()) { fail(r, "invalid_arg"); return r; }
            int64_t ms = std::atoll(d->second.c_str());
            if (ms <= 0 || ms > 120 * 3600 * 1000LL) {
                fail(r, "invalid_arg"); return r;
            }
            std::string label = a.count("label") ? a.at("label") : "";
            std::string id;
            FeatureError e = svc_.createTimer(ms, label, now_mono_ms,
                                              now_wall_ms, &id);
            if (!e.ok) { fail(r, e.code); return r; }
            r.result["id"] = id;
            emit("timer", "create", id, now_mono_ms);
            return r;
        }
        if (req.name == "list_timers") {
            auto ts = svc_.listTimers();
            r.result["count"] = std::to_string(ts.size());
            for (const auto& t : ts)
                r.result["timer"] += (r.result["timer"].empty() ? "" : ",")
                    + t.id + ":" + std::to_string(t.duration_ms);
            return r;
        }
        if (req.name == "cancel_timer") {
            if (!a.count("id")) { fail(r, "invalid_arg"); return r; }
            if (a.at("id") == "all") { fail(r, "confirm_required"); return r; }
            FeatureError e = svc_.cancelTimer(a.at("id"));
            if (!e.ok) { fail(r, e.code); return r; }
            emit("timer", "cancel", a.at("id"), now_mono_ms);
            return r;
        }
        if (req.name == "create_alarm") {
            if (!a.count("hour") || !a.count("minute")) {
                fail(r, "invalid_arg"); return r;
            }
            int64_t h = std::atoll(a.at("hour").c_str());
            int64_t mi = std::atoll(a.at("minute").c_str());
            int rep = a.count("repeat") ? std::atoi(a.at("repeat").c_str()) : 0;
            std::string label = a.count("label") ? a.at("label") : "";
            std::string id;
            FeatureError e = svc_.createAlarm(h, mi, rep, label,
                                              now_wall_ms, &id);
            if (!e.ok) { fail(r, e.code); return r; }
            r.result["id"] = id;
            emit("alarm", "create", id, now_mono_ms);
            return r;
        }
        if (req.name == "list_alarms") {
            auto as = svc_.listAlarms();
            r.result["count"] = std::to_string(as.size());
            for (const auto& al : as)
                r.result["alarm"] += (r.result["alarm"].empty() ? "" : ",")
                    + al.id + ":" + std::to_string(al.hour) + ":"
                    + std::to_string(al.minute);
            return r;
        }
        if (req.name == "enable_alarm") {
            if (!a.count("id") || !a.count("on")) { fail(r, "invalid_arg"); return r; }
            bool on = a.at("on") == "true";
            FeatureError e = svc_.enableAlarm(a.at("id"), on);
            if (!e.ok) { fail(r, e.code); return r; }
            emit("alarm", on ? "enable" : "disable", a.at("id"), now_mono_ms);
            return r;
        }
        if (req.name == "delete_alarm") {
            if (!a.count("id")) { fail(r, "invalid_arg"); return r; }
            if (a.at("id") == "all") { fail(r, "confirm_required"); return r; }
            FeatureError e = svc_.deleteAlarm(a.at("id"));
            if (!e.ok) { fail(r, e.code); return r; }
            emit("alarm", "delete", a.at("id"), now_mono_ms);
            return r;
        }
        if (req.name == "show_clock") {
            svc_.setDisplayMode(DisplayMode::Clock);
            emit("display", "clock", "", now_mono_ms);
            return r;
        }
        if (req.name == "show_pet") {
            svc_.setDisplayMode(DisplayMode::Pet);
            emit("display", "pet", "", now_mono_ms);
            return r;
        }
        // R5_R4 (B2): AI 模式查询/设置 与 停止响铃
        if (req.name == "get_ai_mode") {
            r.result["mode"] = svc_.aiMode();
            return r;
        }
        if (req.name == "set_ai_mode") {
            if (!a.count("mode")) { fail(r, "invalid_arg"); return r; }
            const std::string& m = a.at("mode");
            if (m != "auto" && m != "fast" && m != "deep" && m != "local") {
                fail(r, "invalid_arg"); return r;
            }
            svc_.setAiMode(m);
            r.result["mode"] = svc_.aiMode();
            emit("ai_mode", "set", m, now_mono_ms);
            return r;
        }
        if (req.name == "dismiss_alert") {
            // 停止当前定时器/闹钟响铃 (不删除任何配置)
            svc_.dismissAlert();
            emit("alert", "dismiss", "", now_mono_ms);
            return r;
        }
        if (req.name == "add_note") {
            if (!a.count("text")) { fail(r, "invalid_arg"); return r; }
            std::string id;
            FeatureError e = svc_.addNote(a.at("text"), now_wall_ms, &id);
            if (!e.ok) { fail(r, e.code); return r; }
            r.result["id"] = id;
            emit("note", "add", id, now_mono_ms);
            return r;
        }
        if (req.name == "list_notes") {
            int limit = a.count("limit") ? std::atoi(a.at("limit").c_str()) : 5;
            auto ns = svc_.listNotes(limit);
            r.result["count"] = std::to_string(ns.size());
            for (const auto& n : ns)
                r.result["note"] += (r.result["note"].empty() ? "" : ",")
                    + n.id;
            return r;
        }
        if (req.name == "read_note") {
            if (!a.count("id")) { fail(r, "invalid_arg"); return r; }
            std::string text;
            if (!svc_.readNote(a.at("id"), &text)) { fail(r, "not_found"); return r; }
            r.result["text"] = text;
            return r;
        }
        if (req.name == "delete_note") {
            if (!a.count("id")) { fail(r, "invalid_arg"); return r; }
            if (a.at("id") == "all") { fail(r, "confirm_required"); return r; }
            FeatureError e = svc_.deleteNote(a.at("id"));
            if (!e.ok) { fail(r, e.code); return r; }
            emit("note", "delete", a.at("id"), now_mono_ms);
            return r;
        }
        fail(r, "unknown_tool");
        return r;
    }

    void emit(const std::string& feature, const std::string& action,
              const std::string& item_id, int64_t now_mono_ms) {
        feature_out_.emitted = true;
        feature_out_.feature = feature;
        feature_out_.action = action;
        feature_out_.item_id = item_id;
        feature_out_.monotonic_ms = now_mono_ms;
    }

    LocalFeatureService& svc_;
    std::map<std::string, std::map<std::string, std::string>> seen_;
    FeatureEventOut feature_out_;
};

} // namespace holopet
