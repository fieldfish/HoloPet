/**
 * test_text_pager.cpp — R8_R2 (工作包 E) 回归: 完整回答分页器。
 *
 * 覆盖:
 *   - 短中文一页; 长中文多页;
 *   - 中英混排 + 标点 + 原文换行 + emoji;
 *   - 页面拼接无丢字/无重复 (与原文换行重排一致);
 *   - 不切断 UTF-8 码点 (每行都是合法 UTF-8, 无孤立字节);
 *   - page_index/page_count 正确;
 *   - 空文本 → 0 页;
 *   - 行宽不超限 (安全宽度 24, 与圆屏渲染一致) — 离屏越界的第一道保证。
 */

#include "display/text_pager.hpp"

#include <cstdio>
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
static void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

// 局部 UTF-8 合法性 (与 text_once 同规则; 这里避免跨模块依赖)
static bool utf8_ok(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= s.size()) return false;
        for (size_t k = 1; k <= need; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += need + 1;
    }
    return true;
}

static std::string strip_trailing_newlines(std::string s) {
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

/** 字符流比较: 去掉全部渲染换行后必须与原文逐字节一致 (无丢字/无重复) */
static std::string strip_all_newlines(std::string s) {
    std::string out;
    for (char c : s) if (c != '\n') out += c;
    return out;
}

int main() {
    std::cout << "=== test_text_pager (R8_R2 E) ===\n\n";

    // 1) 短中文一页
    {
        auto pages = paginateText("你好，我是 HoloPet。",
                                  24, 3);
        check(pages.size() == 1, "短中文 → 1 页");
        check(pages[0].count == 1 && pages[0].index == 0,
              "page_index=0 count=1");
        check(joinPages(pages) == strip_trailing_newlines("你好，我是 HoloPet。"),
              "单页拼接无丢字");
    }

    // 2) 长中文多页 + 拼接无损 (无丢字/无重复)
    {
        std::string long_cn;
        for (int i = 0; i < 40; ++i) long_cn += "第" + std::to_string(i) + "句：";
        long_cn += "这就是完整回答的结尾。";
        auto pages = paginateText(long_cn, 24, 3);
        check(pages.size() >= 3, "长中文 → 多页 (>=3)");
        for (size_t i = 0; i < pages.size(); ++i) {
            check(pages[i].index == static_cast<int>(i) &&
                  pages[i].count == static_cast<int>(pages.size()),
                  "页 " + std::to_string(i) + " index/count 正确");
            check(!pages[i].lines.empty() && pages[i].lines.size() <= 3,
                  "页 " + std::to_string(i) + " 行数 1..3");
            check(pages[i].lines.size() ==
                      (i + 1 == pages.size()
                           ? pages[i].lines.size() : (size_t)3) ||
                  i + 1 == pages.size(),
                  "非末页满 3 行");
        }
        const std::string joined = joinPages(pages);
        check(strip_all_newlines(joined) == strip_all_newlines(long_cn),
              "多页字符流 == 原文 (无丢字/无重复/无改序)");
    }

    // 3) 中英混排 + 标点 + 原文换行 + emoji
    {
        const std::string mixed =
            "Hello HoloPet, 你好！\n"
            "emoji 测试：😊🌧️✨\n"
            "last line 完。";
        auto pages = paginateText(mixed, 24, 3);
        check(!pages.empty(), "混排文本有页");
        const std::string joined = joinPages(pages);
        check(joined == strip_trailing_newlines(mixed),
              "混排拼接无损 (含原文换行)");
        bool all_utf8 = true;
        for (const auto& p : pages)
            for (const auto& ln : p.lines)
                if (!utf8_ok(ln)) all_utf8 = false;
        check(all_utf8, "所有行都是合法 UTF-8 (不切断中文/emoji)");
    }

    // 4) 行宽不超限 (安全宽度 24: CJK 1.0 / ASCII 0.5)
    {
        const std::string wide =
            "这句话非常长非常长非常长非常长非常长非常长非常长非常长结束";
        auto pages = paginateText(wide, 24, 3);
        bool ok = true;
        for (const auto& p : pages) {
            for (const auto& ln : p.lines) {
                double w = 0.0;
                for (size_t i = 0; i < ln.size();) {
                    unsigned char c = static_cast<unsigned char>(ln[i]);
                    size_t len = 1; double cw = 0.5;
                    if (c >= 0x80) {
                        if ((c & 0xE0) == 0xC0) len = 2;
                        else if ((c & 0xF0) == 0xE0) len = 3;
                        else if ((c & 0xF8) == 0xF0) len = 4;
                        cw = 1.0;
                    }
                    w += cw;
                    i += len;
                }
                if (w > 24.0 + 1e-9) ok = false;
            }
        }
        check(ok, "每行宽度 <= 24 (圆屏安全区内, 不越界)");
    }

    // 5) 空文本 → 0 页
    {
        auto pages = paginateText("", 24, 3);
        check(pages.empty(), "空文本 → 0 页");
        auto pages2 = wrapAllText("", 24);
        check(pages2.size() == 1 && pages2[0].empty(),
              "wrapAllText(空) 返回单个空行 (调用方按无内容处理)");
    }

    // 6) 恰好 3 行 → 1 页; 4 行 → 2 页
    {
        auto p3 = paginateText("一行\n二行\n三行", 24, 3);
        check(p3.size() == 1, "3 行 → 1 页");
        auto p4 = paginateText("一行\n二行\n三行\n四行", 24, 3);
        check(p4.size() == 2 && p4[1].lines.size() == 1, "4 行 → 2 页");
        check(joinPages(p4) == "一行\n二行\n三行\n四行", "跨页换行保留");
    }

    // 7) 超长无空白 ASCII (模拟"项目回答上限内"压力)
    {
        std::string s(1000, 'a');
        auto pages = paginateText(s, 24, 3);
        check(!pages.empty(), "1000 ASCII 有页");
        check(strip_all_newlines(joinPages(pages)) == s,
              "1000 ASCII 字符流无损");
        // 每行 24 宽 = 48 ASCII/行 → 1000/48 ≈ 21 行 → 7 页
        check(pages.size() == 7, "页数符合宽度模型 (48 ASCII/行, 3 行/页)");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
