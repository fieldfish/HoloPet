/**
 * test_mini_json.cpp — 完整 JSON 解析器测试 (V6-R2)
 * 覆盖: 引号/反斜杠/换行转义/\uXXXX+代理对/Unicode/空串/缺字段/错误类型/
 *       截断 JSON/超长消息/数字语法/数组嵌套/序列化往返。
 */

#include "system/mini_json.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static bool parseOk(const std::string& s, std::string& err_out) {
    ProtocolError err;
    auto v = parseJson(s, &err);
    if (!v) { err_out = err.reason; return false; }
    return true;
}

int main() {
    std::cout << "=== test_mini_json (V6-R2) ===\n\n";
    std::string err;

    // 基础
    check(parseOk(R"({"a":1})", err), "object number");
    check(parseOk(R"([1,"x",true,null,{"k":[]}])", err), "array nested");
    check(parseOk(R"("hello")", err), "bare string");
    check(parseOk("123.5e-2", err), "number exponent");

    // 转义
    {
        auto v = parseJson(R"("a\"b\\c\/d\b\f\n\r\t")", nullptr);
        check(v && v->str == "a\"b\\c/d\b\f\n\r\t", "全部转义字符");
    }
    // \uXXXX + 代理对
    {
        auto v = parseJson(R"("\u4f60\u597d")", nullptr);
        check(v && v->str == "你好", "\\uXXXX CJK");
        auto e = parseJson(R"("\ud83d\ude00")", nullptr);   // U+1F600
        check(e && e->str == "\xF0\x9F\x98\x80", "代理对 → UTF-8");
    }
    // 未配对代理
    check(!parseOk(R"("\ud800")", err), "unpaired high surrogate 拒绝");
    check(!parseOk(R"("\udc00")", err), "unpaired low surrogate 拒绝");

    // 空字符串
    {
        auto v = parseJson(R"("")", nullptr);
        check(v && v->str.empty(), "空字符串");
    }
    // 缺字段 (访问器)
    {
        auto v = parseJson(R"({"a":1})", nullptr);
        check(v && !v->getString("missing") && v->getNumber("missing") == std::nullopt,
              "缺失字段访问 → nullopt");
        check(v && v->getNumber("a") == 1.0, "数字访问");
        check(v && v->getString("a") == std::nullopt, "错误类型 → nullopt");
    }
    // 错误 JSON
    check(!parseOk("{", err), "截断 object 拒绝");
    check(!parseOk(R"({"a":})", err), "截断 value 拒绝");
    check(!parseOk(R"("abc)", err), "未终止字符串拒绝");
    check(!parseOk(R"({"a" 1})", err), "缺冒号拒绝");
    check(!parseOk("01", err), "前导零拒绝");
    check(!parseOk("tru", err), "坏字面量拒绝");
    check(!parseOk("{} x", err), "尾随字符拒绝");

    // 超长消息 (解析器本身无长度上限 — 由调用方限制; 此处证明大消息不崩溃)
    {
        std::string big(64 * 1024, 'x');
        std::string json = "[\"" + big + "\"]";
        auto v = parseJson(json, nullptr);
        check(v && v->arr.size() == 1 && v->arr[0].str == big, "64KiB 字符串不崩溃");
    }

    // 序列化往返
    {
        auto v = parseJson(R"({"b":true,"n":1.5,"s":"x\"y","a":[1,2]})", nullptr);
        check(v.has_value(), "roundtrip 解析");
        auto s = serializeJson(*v);
        auto v2 = parseJson(s, nullptr);
        check(v2.has_value(), "roundtrip 再解析");
        check(serializeJson(*v2) == s, "roundtrip 稳定");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
