#pragma once
/**
 * square_menu_renderer.hpp — 方形列表菜单渲染 (纯渲染组件)。
 *
 * 约束: 黑底; 主蓝 64,160,255 / 次级蓝 32,128,216; 禁白色/渐变/辉光/
 * 实心高亮块; 外层细蓝色圆角矩形边框; 顶部标题"功能菜单"; 右上位置 n/5;
 * 五项蓝色线性图标+中文标签; 当前项细蓝色圆角空心框; 右侧细滚动条;
 * 底部"旋转选择 · 短按确认 · 长按返回"; ASCII/CJK 双字体分流。
 * 只读 MenuSnapshot; font==nullptr 跳过文字 (确定性几何测试)。
 */
#include "display/text_wrap.hpp"
#include "ui/menu_controller.hpp"

#ifdef _WIN32
#define NOMINMAX
#endif
#include <SDL.h>
#include <SDL_ttf.h>

#include <cmath>
#include <string>
#include <vector>

namespace holopet {

inline constexpr SDL_Color kSquarePrimary = {64, 160, 255, 255};
inline constexpr SDL_Color kSquareSecondary = {32, 128, 216, 255};
inline constexpr SDL_Color kSquareBackdrop = {0, 0, 0, 255};

namespace sq_detail {

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
}

inline void roundedFrame(SDL_Renderer* r, int x0, int y0, int x1, int y1,
                         int rad = 12) {
    const double step = 3.14159265358979323846 / 12.0;
    auto arc = [&](int cxx, int cyy, double a0, double a1) {
        for (double a = a0; a < a1 + 1e-9; a += step) {
            SDL_RenderDrawLineF(
                r, static_cast<float>(cxx + rad * std::cos(a)),
                static_cast<float>(cyy + rad * std::sin(a)),
                static_cast<float>(cxx + rad * std::cos(a + step)),
                static_cast<float>(cyy + rad * std::sin(a + step)));
        }
    };
    SDL_RenderDrawLineF(r, static_cast<float>(x0 + rad),
                        static_cast<float>(y0),
                        static_cast<float>(x1 - rad),
                        static_cast<float>(y0));
    SDL_RenderDrawLineF(r, static_cast<float>(x0 + rad),
                        static_cast<float>(y1),
                        static_cast<float>(x1 - rad),
                        static_cast<float>(y1));
    SDL_RenderDrawLineF(r, static_cast<float>(x0),
                        static_cast<float>(y0 + rad),
                        static_cast<float>(x0),
                        static_cast<float>(y1 - rad));
    SDL_RenderDrawLineF(r, static_cast<float>(x1),
                        static_cast<float>(y0 + rad),
                        static_cast<float>(x1),
                        static_cast<float>(y1 - rad));
    arc(x0 + rad, y0 + rad, 3.14159265358979323846,
        1.5 * 3.14159265358979323846);
    arc(x1 - rad, y0 + rad, 1.5 * 3.14159265358979323846, 0.0);
    arc(x1 - rad, y1 - rad, 0.0, 0.5 * 3.14159265358979323846);
    arc(x0 + rad, y1 - rad, 0.5 * 3.14159265358979323846,
        3.14159265358979323846);
}

struct Run { std::string text; bool ascii; };
inline std::vector<Run> splitRuns(const std::string& text) {
    std::vector<Run> out;
    std::string cur;
    bool cur_ascii = false, first = true;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        const bool is_ascii = (c < 0x80);
        if (first) { cur_ascii = is_ascii; first = false; }
        else if (is_ascii != cur_ascii) {
            out.push_back({cur, cur_ascii});
            cur.clear();
            cur_ascii = is_ascii;
        }
        cur.append(text, i, len);
        i += len;
    }
    if (!cur.empty()) out.push_back({cur, cur_ascii});
    return out;
}

/** 双字体文本: 返回绘制宽度; font==nullptr → 0 (几何测试模式)。 */
inline int textWidth(TTF_Font* font, TTF_Font* ascii, const std::string& text) {
    if (!font || text.empty()) return 0;
    int total = 0;
    for (const auto& run : splitRuns(text)) {
        TTF_Font* f = run.ascii ? ascii : font;
        if (!f) continue;
        SDL_Surface* s = TTF_RenderUTF8_Blended(f, run.text.c_str(),
                                                kSquarePrimary);
        if (s) { total += s->w; SDL_FreeSurface(s); }
    }
    return total;
}

inline void textAt(SDL_Renderer* r, TTF_Font* font, TTF_Font* ascii,
                   SDL_Color color, const std::string& text, int x, int y) {
    if (!font || text.empty()) return;
    for (const auto& run : splitRuns(text)) {
        TTF_Font* f = run.ascii ? ascii : font;
        if (!f) continue;
        SDL_Surface* s = TTF_RenderUTF8_Blended(f, run.text.c_str(), color);
        if (!s) continue;
        SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
        SDL_Rect dst{x, y, s->w, s->h};
        SDL_RenderCopy(r, t, nullptr, &dst);
        x += s->w;
        SDL_DestroyTexture(t);
        SDL_FreeSurface(s);
    }
}

inline void textCentered(SDL_Renderer* r, TTF_Font* font, TTF_Font* ascii,
                         SDL_Color color, const std::string& text,
                         int cx, int y) {
    textAt(r, font, ascii, color, text, cx - textWidth(font, ascii, text) / 2,
           y);
}

/** 五项线性图标 (同笔画宽、主蓝): 0时钟 1定时器 2闹钟 3便签 4AI */
inline void drawIcon(SDL_Renderer* r, int kind, int cx, int cy, int rad) {
    setColor(r, kSquarePrimary);
    switch (kind) {
    case 0: {  // 时钟: 圆 + 两针
        for (int i = 0; i < 40; ++i) {
            const double a0 = 2 * 3.14159265358979323846 * i / 40.0;
            const double a1 = 2 * 3.14159265358979323846 * (i + 1) / 40.0;
            SDL_RenderDrawLineF(r,
                static_cast<float>(cx + rad * std::cos(a0)),
                static_cast<float>(cy + rad * std::sin(a0)),
                static_cast<float>(cx + rad * std::cos(a1)),
                static_cast<float>(cy + rad * std::sin(a1)));
        }
        SDL_RenderDrawLineF(r, static_cast<float>(cx),
                            static_cast<float>(cy),
                            static_cast<float>(cx),
                            static_cast<float>(cy - rad * 0.6f));
        SDL_RenderDrawLineF(r, static_cast<float>(cx),
                            static_cast<float>(cy),
                            static_cast<float>(cx + rad * 0.5f),
                            static_cast<float>(cy + rad * 0.3f));
        break; }
    case 1: {  // 定时器: 圆 + 弧 + 顶部旋钮
        for (int i = 0; i < 40; ++i) {
            const double a0 = 2 * 3.14159265358979323846 * i / 40.0;
            const double a1 = 2 * 3.14159265358979323846 * (i + 1) / 40.0;
            SDL_RenderDrawLineF(r,
                static_cast<float>(cx + rad * std::cos(a0)),
                static_cast<float>(cy + rad * std::sin(a0)),
                static_cast<float>(cx + rad * std::cos(a1)),
                static_cast<float>(cy + rad * std::sin(a1)));
        }
        SDL_RenderDrawLineF(r, static_cast<float>(cx),
                            static_cast<float>(cy),
                            static_cast<float>(cx),
                            static_cast<float>(cy - rad * 0.7f));
        SDL_RenderDrawLineF(r, static_cast<float>(cx - rad * 0.3f),
                            static_cast<float>(cy - rad * 0.9f),
                            static_cast<float>(cx + rad * 0.3f),
                            static_cast<float>(cy - rad * 0.9f));
        break; }
    case 2: {  // 闹钟: 铃身弧 + 底边 + 铃锤
        for (int i = 0; i < 20; ++i) {
            const double a0 = 3.14159265358979323846 * i / 20.0;
            const double a1 = 3.14159265358979323846 * (i + 1) / 20.0;
            SDL_RenderDrawLineF(r,
                static_cast<float>(cx - rad + rad * std::cos(a0)),
                static_cast<float>(cy - rad * 0.4f + rad * 0.7f
                    * std::sin(a0)),
                static_cast<float>(cx - rad + rad * std::cos(a1)),
                static_cast<float>(cy - rad * 0.4f + rad * 0.7f
                    * std::sin(a1)));
        }
        SDL_RenderDrawLineF(r, static_cast<float>(cx - rad),
                            static_cast<float>(cy - rad * 0.4f),
                            static_cast<float>(cx - rad),
                            static_cast<float>(cy + rad * 0.2f));
        SDL_RenderDrawLineF(r, static_cast<float>(cx + rad),
                            static_cast<float>(cy - rad * 0.4f),
                            static_cast<float>(cx + rad),
                            static_cast<float>(cy + rad * 0.2f));
        SDL_RenderDrawLineF(r, static_cast<float>(cx - rad),
                            static_cast<float>(cy + rad * 0.2f),
                            static_cast<float>(cx + rad),
                            static_cast<float>(cy + rad * 0.2f));
        SDL_RenderDrawLineF(r, static_cast<float>(cx),
                            static_cast<float>(cy + rad * 0.2f),
                            static_cast<float>(cx),
                            static_cast<float>(cy + rad * 0.55f));
        break; }
    case 3: {  // 便签: 圆角片 + 三行
        roundedFrame(r, cx - rad, cy - rad, cx + rad, cy + rad, 4);
        for (int i = 0; i < 3; ++i) {
            SDL_RenderDrawLineF(r,
                static_cast<float>(cx - rad + 4),
                static_cast<float>(cy - rad + 5 + i * 5),
                static_cast<float>(cx + rad - 4),
                static_cast<float>(cy - rad + 5 + i * 5));
        }
        break; }
    default: {  // AI: 芯片框 + 触点 + 内线
        roundedFrame(r, cx - rad, cy - rad, cx + rad, cy + rad, 4);
        for (int s : {-1, 1}) {
            SDL_RenderDrawLineF(r,
                static_cast<float>(cx + s * rad),
                static_cast<float>(cy - rad / 2),
                static_cast<float>(cx + s * rad),
                static_cast<float>(cy + rad / 2));
        }
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - rad / 2), static_cast<float>(cy),
            static_cast<float>(cx + rad / 2), static_cast<float>(cy));
        break; }
    }
}

}  // namespace sq_detail

/** 方形列表菜单主渲染入口。size=画布边长; snap=菜单快照。 */
inline void renderSquareMenu(SDL_Renderer* r, int size,
                             const MenuSnapshot& snap, TTF_Font* font,
                             TTF_Font* ascii_font) {
    sq_detail::setColor(r, kSquareBackdrop);
    SDL_RenderClear(r);

    const int m = size / 20;             // 外框边距
    const int fx0 = m, fy0 = m, fx1 = size - m, fy1 = size - m;
    const int cx = size / 2;

    sq_detail::setColor(r, kSquarePrimary);
    sq_detail::roundedFrame(r, fx0, fy0, fx1, fy1, 24);
    sq_detail::roundedFrame(r, fx0 + 1, fy0 + 1, fx1 - 1, fy1 - 1, 23);

    // 标题 + 位置 n/5
    sq_detail::textCentered(r, font, ascii_font, kSquarePrimary,
                            snap.title.empty() ? "功能菜单" : snap.title,
                            cx, fy0 + 18);
    const std::string pos = std::to_string(snap.selected_index + 1) + "/"
        + std::to_string((std::max)(1, static_cast<int>(snap.items.size())));
    sq_detail::textAt(r, font, ascii_font, kSquareSecondary, pos,
                      fx1 - m - sq_detail::textWidth(font, ascii_font, pos),
                      fy0 + 18);

    const int rows_y0 = fy0 + 62;
    const int rows_h = (fy1 - 56) - rows_y0;
    const int n = static_cast<int>(snap.items.size());

    if (n == 0) {
        // 编辑页: 大号数值
        std::string v;
        switch (snap.layer) {
            case MenuLayer::TimerCreate:
                v = std::to_string(snap.edit_minutes) + " 分钟";
                break;
            case MenuLayer::AlarmCreateHour:
                v = std::to_string(snap.edit_hour) + " 时";
                break;
            case MenuLayer::AlarmCreateMinute:
                v = std::to_string(snap.edit_minute) + " 分";
                break;
            case MenuLayer::AlarmCreateRepeat:
                v = (snap.edit_repeat == 1) ? "每日"
                    : (snap.edit_repeat == 2) ? "工作日" : "一次性";
                break;
            default:
                break;
        }
        sq_detail::textCentered(r, font, ascii_font, kSquarePrimary, v,
                                cx, rows_y0 + rows_h / 2 - 12);
    } else {
        const int row_h = rows_h / (std::max)(n, 5);
        const int row_x0 = fx0 + 18;
        const int row_x1 = fx1 - 26;
        const int layer_icon = [&]() -> int {
            switch (snap.layer) {
                case MenuLayer::TimerList: return 1;
                case MenuLayer::AlarmList: return 2;
                case MenuLayer::NoteList:  return 3;
                case MenuLayer::AiMode:    return 4;
                default: return -1;   // Root: 逐项图标
            }
        }();
        for (int i = 0; i < n; ++i) {
            const int ry = rows_y0 + i * row_h;
            const bool sel = (i == snap.selected_index);
            // 图标 (最左; 列表页用层固定图标, 根菜单逐项)
            sq_detail::drawIcon(r, layer_icon >= 0 ? layer_icon : i,
                                row_x0 + 22, ry + row_h / 2, 15);
            // 标签
            sq_detail::textAt(r, font, ascii_font,
                              sel ? kSquarePrimary : kSquareSecondary,
                              snap.items[i], row_x0 + 52, ry + row_h / 2 - 11);
            // 选中: 细圆角空心框
            if (sel) {
                sq_detail::setColor(r, kSquarePrimary);
                sq_detail::roundedFrame(r, row_x0 + 8, ry + 6,
                                        row_x1 - 8, ry + row_h - 6, 12);
                sq_detail::roundedFrame(r, row_x0 + 9, ry + 7,
                                        row_x1 - 9, ry + row_h - 7, 12);
            }
        }
        // 右侧细滚动条
        if (n > 1) {
            sq_detail::setColor(r, kSquareSecondary);
            const int sb_h = (std::max)(12, rows_h / n);
            const int sb_y = rows_y0 + snap.selected_index * row_h
                + row_h / 2 - sb_h / 2;
            SDL_RenderDrawLineF(r, static_cast<float>(fx1 - 12),
                                static_cast<float>(sb_y),
                                static_cast<float>(fx1 - 12),
                                static_cast<float>(sb_y + sb_h));
        }
    }

    // 便签正文预览 (R8_R4_R3_R4_R5 §4.4: 安全区换行/截断 + 页码)
    if (!snap.preview_text.empty()) {
        auto lines = wrapText(snap.preview_text, 18, 3);
        int py = rows_y0 + 10;
        for (size_t i = 0; i < lines.size(); ++i) {
            sq_detail::textAt(r, font, ascii_font, kSquarePrimary,
                              lines[i], fx0 + 40, py);
            py += 30;
        }
        sq_detail::textCentered(r, font, ascii_font, kSquareSecondary,
                                std::to_string(lines.size()) + " 行",
                                cx, fy1 - 52);
    }

    // 底部操作提示
    sq_detail::textCentered(r, font, ascii_font, kSquareSecondary,
                            "旋转选择 · 短按确认 · 长按返回",
                            cx, fy1 - 26);
}

/** 公开居中文本入口 (时钟等复用; 双字体分流)。 */
inline void renderSquareTextCentered(SDL_Renderer* r, TTF_Font* cjk,
                                     TTF_Font* ascii, SDL_Color color,
                                     const std::string& text,
                                     int cx, int y) {
    sq_detail::textCentered(r, cjk, ascii, color, text, cx, y);
}

}  // namespace holopet
