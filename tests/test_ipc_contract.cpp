/**
 * test_ipc_contract.cpp — R4 P1: 跨语言 IPC v1 合同测试 (C++ 侧)
 *
 * 与 Python (agent/holopet_agentd/tests/test_protocol_v4.py) 读取同一份
 * tests/fixtures/ipc_v1/cases.json, 对每条 case 做相同断言:
 *  - line case: parseWorkerLine → 期望 ok 或稳定错误码
 *  - stream case: LineBuffer 拆行 → 每行 parseWorkerLine 结果一致,
 *    overflow 与 Python read_lines 一致
 * 错误码 (两侧一致): bad_json / not_object / bad_version /
 *                    bad_request_id / unknown_type / line_too_long
 */

#include "ipc/ai_worker_client.hpp"   // LineBuffer
#include "ipc/ai_worker_protocol.hpp" // parseWorkerLine
#include "system/mini_json.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

// ---- base64 解码 (fixture input 用) ----
static const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64decode(const std::string& in) {
    std::string out;
    // R4-R2 (F3): unsigned + 掩码 — 修复长输入下 int 溢出 UB 导致
    // 部分长 fixture 解码损坏 (valid_4byte 误报 bad_json 的根因)
    unsigned int val = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const char* p = kB64;
        while (*p && *p != c) ++p;
        if (!*p) continue;
        val = (val << 6) | static_cast<unsigned int>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xFFu));
            val &= (bits == 0) ? 0u : ((1u << bits) - 1u);   // 只保留剩余位
        }
    }
    return out;
}

/** line case 期望: true=ok; 否则为错误码字符串 */
struct Expect {
    bool ok = false;
    std::string code;
};

int main(int argc, char* argv[]) {
    std::cout << "=== test_ipc_contract (V6 R4, C++ side) ===\n\n";

    const std::string fixture_path = (argc > 1)
        ? argv[1] : "tests/fixtures/ipc_v1/cases.json";
    std::ifstream f(fixture_path);
    if (!f) {
        std::cout << "  FAIL: fixture 不可读: " << fixture_path << '\n';
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    auto root = parseJson(ss.str());
    if (!root || root->type != JsonValue::Type::Object) {
        std::cout << "  FAIL: fixture JSON 解析失败\n";
        return 1;
    }
    auto* v = root->get("protocol_version");
    check(v && v->type == JsonValue::Type::String && v->str == "1",
          "fixture 协议版本 = 1");
    auto* cases = root->get("cases");
    if (!cases || cases->type != JsonValue::Type::Array) {
        std::cout << "  FAIL: fixture 缺 cases 数组\n";
        return 1;
    }

    size_t total = 0;
    for (const auto& c : cases->arr) {
        const std::string name = c.getString("name").value_or("?");
        const std::string kind = c.getString("kind").value_or("?");
        const std::string b64 = c.getString("input_b64").value_or("");
        const std::string b64_2 = c.getString("chunk2_b64").value_or("");
        const bool synthetic_overlong =
            c.get("synthetic") && c.get("synthetic")->type == JsonValue::Type::String
            && c.get("synthetic")->str == "overlong_partial";

        if (kind == "line") {
            // ---- 单行解析 (C++ 是事件接收方: 取 expect.event) ----
            const std::string line = b64decode(b64);
            auto res = parseWorkerLine(line);
            const JsonValue* exp = c.get("expect");
            if (exp && exp->type == JsonValue::Type::Object) {
                exp = exp->get("event");   // 方向分期望
            }
            bool expect_ok = exp && exp->type == JsonValue::Type::Bool && exp->b;
            std::string expect_code = (exp && exp->type == JsonValue::Type::String)
                ? exp->str : "";
            bool ok = expect_ok
                ? (res.message.has_value() && !res.error.has_value())
                : (res.error.has_value() && !expect_code.empty()
                   && res.error->reason == expect_code);
            check(ok, "line[" + name + "] " +
                  (expect_ok ? "ok" : ("code=" + expect_code)));
            if (!ok && res.error) {
                std::cout << "    detail: got error=" << res.error->reason
                          << " expect=" << (expect_ok ? "ok" : expect_code)
                          << " line_bytes=" << line.size() << '\n';
            }
            ++total;
        } else if (kind == "stream") {
            // ---- 流式拆行 ----
            LineBuffer lb;
            bool overflow = false;
            const std::string synth = (c.get("synthetic") &&
                c.get("synthetic")->type == JsonValue::Type::String)
                ? c.get("synthetic")->str : "";
            if (synthetic_overlong) {
                std::string big(LineBuffer::kMaxLineBytes + 10, 'a');
                overflow = lb.append(big.data(), big.size());
            } else if (synth == "legal_then_overlong_tail") {
                std::string legal =
                    "{\"v\":\"1\",\"request_id\":\"a\",\"type\":\"done\"}\n";
                std::string big(LineBuffer::kMaxLineBytes + 10, 'b');
                std::string all = legal + big;
                overflow = lb.append(all.data(), all.size());   // 合法行必须保留
            } else if (synth == "overlong_line_with_newline") {
                std::string big(LineBuffer::kMaxLineBytes + 10, 'c');
                big.push_back('\n');
                overflow = lb.append(big.data(), big.size());
            } else if (synth == "exact_max_partial") {
                std::string exact(LineBuffer::kMaxLineBytes, 'd');
                overflow = lb.append(exact.data(), exact.size());
            } else if (synth == "max_plus_one_partial") {
                std::string big(LineBuffer::kMaxLineBytes + 1, 'e');
                overflow = lb.append(big.data(), big.size());
            } else {
                std::string in = b64decode(b64);
                overflow = lb.append(in.data(), in.size());
                if (!b64_2.empty()) {
                    std::string in2 = b64decode(b64_2);
                    overflow = overflow || lb.append(in2.data(), in2.size());
                }
            }
            std::vector<WorkerParseResult> results;
            for (auto line = lb.popLine(); line; line = lb.popLine()) {
                results.push_back(parseWorkerLine(*line));
            }
            auto* exp_arr = c.get("expect");
            bool expect_overflow = c.get("overflow") &&
                c.get("overflow")->type == JsonValue::Type::Bool &&
                c.get("overflow")->b;
            size_t exp_count = (exp_arr && exp_arr->type == JsonValue::Type::Array)
                ? exp_arr->arr.size() : 0;
            bool ok = (overflow == expect_overflow) && (results.size() == exp_count);
            for (size_t i = 0; ok && i < exp_count; ++i) {
                const JsonValue& e = exp_arr->arr[i];
                bool exp_ok = e.type == JsonValue::Type::Bool && e.b;
                if (exp_ok) ok = results[i].message.has_value();
                else ok = results[i].error.has_value() &&
                          results[i].error->reason == e.str;
            }
            check(ok, "stream[" + name + "] overflow=" +
                  (overflow ? "1" : "0") + " lines=" +
                  std::to_string(results.size()) + "/" +
                  std::to_string(exp_count));
            ++total;
        }
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 (共 " << total << " case) ===\n";
    return g_failed == 0 ? 0 : 1;
}
