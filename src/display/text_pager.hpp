#pragma once
/**
 * text_pager.hpp — R8_R2 (工作包 E): 完整回答分页器 (纯逻辑, 可单测)
 *
 * 目标: 把 ConversationController 已提交的完整回答切成多页, 保证
 *   - 以 UTF-8 字符边界换行/分页 (不切断中文或 emoji 字节序列);
 *   - 所有页按序拼接 == 完整回答 (允许渲染换行, 不允许丢字/重复);
 *   - 提供 page_index / page_count;
 *   - 空文本 → 0 页 (调用方按"无回答"处理)。
 *
 * 行宽按与 wrapText 相同的宽度模型: CJK/全角计 1.0, ASCII 计 0.5。
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace holopet {

struct TextPage {
    std::vector<std::string> lines;   // 本页各行 (不含页码)
    int index = 0;                    // 0-based
    int count = 1;                    // 总页数
};

/** 全文按宽度换行 (无行数截断; 保留原文换行为强制断行点) */
inline std::vector<std::string> wrapAllText(const std::string& text,
                                            int max_chars_per_line) {
    std::vector<std::string> lines;
    if (max_chars_per_line <= 0) return lines;
    const double limit = static_cast<double>(max_chars_per_line) + 1e-9;

    auto flush = [&]() { lines.push_back(std::string()); };
    flush();                                   // 至少一个当前行

    size_t i = 0;
    double width = 0.0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t code_len = 1;
        double w = 0.5;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0)      code_len = 2;
            else if ((c & 0xF0) == 0xE0) code_len = 3;
            else if ((c & 0xF8) == 0xF0) code_len = 4;
            w = 1.0;
        }
        if (code_len > text.size() - i) code_len = text.size() - i;  // 防御
        if (text[i] == '\n') {                 // 原文换行 = 强制断行
            lines.push_back(std::string());
            width = 0.0;
            i += 1;
            continue;
        }
        if (width + w > limit && !lines.back().empty()) {
            lines.push_back(std::string());
            width = 0.0;
        }
        lines.back().append(text, i, code_len);
        width += w;
        i += code_len;
    }
    return lines;
}

/**
 * 分页: 每页最多 max_lines_per_page 行。
 * 空文本 → 空 vector (0 页)。拼接所有页的全部行 (按 \n 连接) 再加回
 * 被剔除的结尾空行 == 原文 (换行重排)。
 */
inline std::vector<TextPage> paginateText(const std::string& text,
                                          int max_chars_per_line,
                                          int max_lines_per_page) {
    std::vector<TextPage> pages;
    if (text.empty() || max_lines_per_page <= 0) return pages;
    auto lines = wrapAllText(text, max_chars_per_line);
    // 尾随空行 (由结尾 \n 产生) 不计入分页内容, 但拼接还原时属于换行符
    size_t content_lines = lines.size();
    while (content_lines > 1 && lines[content_lines - 1].empty())
        --content_lines;

    const int per = max_lines_per_page;
    const int count = static_cast<int>(
        (content_lines + static_cast<size_t>(per) - 1) / static_cast<size_t>(per));
    pages.reserve(static_cast<size_t>(count));
    for (int p = 0; p < count; ++p) {
        TextPage page;
        page.index = p;
        page.count = count;
        const size_t begin = static_cast<size_t>(p) * static_cast<size_t>(per);
        const size_t end = (std::min)(begin + static_cast<size_t>(per),
                                      content_lines);
        for (size_t k = begin; k < end; ++k) page.lines.push_back(lines[k]);
        pages.push_back(std::move(page));
    }
    return pages;
}

/** 拼接还原 (测试用): 所有页行以 \n 连接 == 原文去掉尾随空白换行后的重排 */
inline std::string joinPages(const std::vector<TextPage>& pages) {
    std::string out;
    bool first = true;
    for (const auto& p : pages) {
        for (const auto& ln : p.lines) {
            if (!first) out += "\n";
            out += ln;
            first = false;
        }
    }
    return out;
}

} // namespace holopet
