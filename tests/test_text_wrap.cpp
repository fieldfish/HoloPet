/**
 * test_text_wrap.cpp — V6 圆屏文字安全换行测试
 */

#include "display/text_wrap.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_text_wrap (V6) ===\n\n";

    // 短文本单行
    {
        auto lines = wrapText("你好", 10, 3);
        check(lines.size() == 1 && lines[0] == "你好", "短文本单行");
    }

    // 中文按全宽计
    {
        auto lines = wrapText("一二三四五六七八九十", 5, 3);
        check(lines.size() == 2, "10 个全宽字 → 2 行");
        check(lines[0] == "一二三四五", "每行 5 字");
    }

    // ASCII 半宽
    {
        auto lines = wrapText("abcdefgh", 2, 3);   // 2 chars * 0.5 = 4 ascii/行
        check(lines.size() == 2, "8 ascii / 4 = 2 行");
        check(lines[0] == "abcd", "第一行 4 字符");
    }

    // 行数上限截断 + 省略号
    {
        auto lines = wrapText("一二三四五六七八九十一二三四五六七八九十", 4, 2);
        check(lines.size() == 2, "超过 max_lines 截断");
        check(lines[1].size() >= 3 &&
              lines[1].compare(lines[1].size() - 3, 3, "\xE2\x80\xA6") == 0,
              "最后一行以 … 结尾");
    }

    // 空输入
    {
        auto lines = wrapText("", 10, 3);
        check(lines.empty(), "空输入无行");
    }

    // 非法参数
    {
        check(wrapText("abc", 0, 3).empty(), "max_chars=0 → 空");
        check(wrapText("abc", 10, 0).empty(), "max_lines=0 → 空");
    }

    // 长词不拆分崩溃 (连续 ASCII)
    {
        auto lines = wrapText("supercalifragilisticexpialidocious", 6, 3);
        check(lines.size() == 3, "长词可安全分行");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
