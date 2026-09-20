#pragma once
/**
 * text_wrap.hpp — 圆屏安全区文字换行 (纯 C++, 可单测)  V6
 *
 * 按字符数估算 (CJK 按全宽计) 把长文本切成多行, 保证不溢出安全区。
 */

#include <string>
#include <vector>

namespace holopet {

/**
 * 安全换行
 * @param text         原始文本
 * @param max_chars    每行最大字符数 (CJK 计 1, ASCII 计 0.5)
 * @param max_lines    最多行数 (超出截断并以 "…" 结尾)
 */
inline std::vector<std::string> wrapText(const std::string& text,
                                         int max_chars,
                                         int max_lines = 3) {
    std::vector<std::string> lines;
    if (max_chars <= 0 || max_lines <= 0) return lines;

    std::string current;
    double width = 0.0;

    auto flush = [&](bool ellipsis) {
        if (ellipsis && !current.empty()) {
            current += "…";
        }
        lines.push_back(current);
        current.clear();
        width = 0.0;
    };

    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int code_len = 1;
        double w = 0.5;   // ASCII 半宽
        if (c >= 0x80) {
            // UTF-8 码点: 前导字节 + 后继字节, 整个码点计全宽 1.0
            if ((c & 0xE0) == 0xC0)       code_len = 2;
            else if ((c & 0xF0) == 0xE0)  code_len = 3;
            else if ((c & 0xF8) == 0xF0)  code_len = 4;
            w = 1.0;
        }
        if (width + w > static_cast<double>(max_chars) + 1e-9) {
            if (static_cast<int>(lines.size()) >= max_lines - 1) {
                flush(true);   // 最后一行截断
                return lines;
            }
            flush(false);
        }
        current.append(text, i, static_cast<size_t>(code_len));
        width += w;
        i += static_cast<size_t>(code_len);
    }
    if (!current.empty()) flush(false);
    return lines;
}

} // namespace holopet
