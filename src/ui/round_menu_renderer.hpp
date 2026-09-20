#pragma once
/**
 * round_menu_renderer.hpp — R8_R4_R3_R4 圆形纵向轮播菜单渲染 (纯渲染组件)。
 *
 * 视觉基准: 桌面 HoloPet_R8_R4_菜单界面参考板_v1.png (唯一采用稿)。
 * 约束 (§2.3):
 *   黑底 (0,0,0); 仅蓝线/蓝字 (主蓝 64,160,255 / 次级蓝 32,128,216);
 *   禁白色/渐变/辉光/实心高亮块/方形外框/滚动条/满屏图标/装饰 HUD。
 *   重要内容位于中心直径 80% 安全区; 主要线宽 >= 3px (逻辑 720 画布);
 *   外圆边框细、连续; 选中项 = 蓝色细圆角空心框。
 * 根菜单纵向轮播: 上一项(小/次级蓝) + 当前项(大/主蓝/细圆角框) +
 *   下一项(小/次级蓝); 顶部标题"功能"; 上下小型蓝色箭头。
 *
 * 只接收不可变快照与画布信息; 不访问功能数据/网络/持久化。
 * font==nullptr 时跳过文字绘制 (确定性几何测试模式)。
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

namespace holopet {

inline constexpr SDL_Color kMenuPrimary = {64, 160, 255, 255};
inline constexpr SDL_Color kMenuSecondary = {32, 128, 216, 255};
inline constexpr SDL_Color kMenuBackdrop = {0, 0, 0, 255};

namespace detail {

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
}

/** 空心圆 (多边形折线, 线宽 1 — 由调用方控制缩放, 外圈细而连续)。 */
inline void ringCircle(SDL_Renderer* r, int cx, int cy, int radius,
                       int segments = 96) {
    SDL_FPoint pts[97];
    for (int i = 0; i < segments; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * i / segments;
        pts[i].x = static_cast<float>(cx + radius * std::cos(a));
        pts[i].y = static_cast<float>(cy + radius * std::sin(a));
    }
    pts[segments] = pts[0];
    SDL_RenderDrawLinesF(r, pts, segments + 1);
}

/** 细圆角空心框 (直线+四角小圆弧, 线宽 1 — 由调用方重复偏移加粗)。 */
inline void roundedFrame(SDL_Renderer* r, int x0, int y0, int x1, int y1,
                         int rad = 12) {
    const double step = 3.14159265358979323846 / 12.0;
    auto arc = [&](int cx, int cy, double a0, double a1) {
        for (double a = a0; a < a1 + 1e-9; a += step) {
            SDL_RenderDrawLineF(
                r, static_cast<float>(cx + rad * std::cos(a)),
                static_cast<float>(cy + rad * std::sin(a)),
                static_cast<float>(cx + rad * std::cos(a + step)),
                static_cast<float>(cy + rad * std::sin(a + step)));
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

namespace detail_run {
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
}  // namespace detail_run

/** 居中单行文本 — ASCII 用 ascii_font, 其余用 cjk font (R8_R4_R3_R4:
 * DroidSansFallbackFull 无 ASCII 字形, 单字体路径数字会变豆腐块)。
 * font==nullptr 跳过 (确定性几何测试); ascii_font==nullptr 退单字体。 */
inline void centerText(SDL_Renderer* r, TTF_Font* font, TTF_Font* ascii_font,
                       SDL_Color color, const std::string& text,
                       int cx, int y) {
    if (!font || text.empty()) return;
    if (!ascii_font) {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_Rect dst{cx - surf->w / 2, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
        return;
    }
    int total = 0;
    for (const auto& run : detail_run::splitRuns(text)) {
        TTF_Font* f = run.ascii ? ascii_font : font;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, run.text.c_str(), color);
        if (!surf) continue;
        total += surf->w;
        SDL_FreeSurface(surf);
    }
    int x = cx - total / 2;
    for (const auto& run : detail_run::splitRuns(text)) {
        TTF_Font* f = run.ascii ? ascii_font : font;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, run.text.c_str(), color);
        if (!surf) continue;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_Rect dst{x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, nullptr, &dst);
        x += surf->w;
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
    }
}

/** 小型蓝色箭头 (上/下三角, 空心线)。 */
inline void arrow(SDL_Renderer* r, int cx, int y, int dir /*+1下 -1上*/,
                  int half = 10) {
    const int h = 8 * dir;
    SDL_RenderDrawLineF(r, static_cast<float>(cx - half),
                        static_cast<float>(y), static_cast<float>(cx),
                        static_cast<float>(y + h));
    SDL_RenderDrawLineF(r, static_cast<float>(cx),
                        static_cast<float>(y + h),
                        static_cast<float>(cx + half),
                        static_cast<float>(y));
}

}  // namespace detail

/** 公开的居中文本入口 (时钟等场景复用; 同样走 ASCII/CJK 双字体分流)。 */
inline void renderRoundTextCentered(SDL_Renderer* r, TTF_Font* cjk,
                                    TTF_Font* ascii, SDL_Color color,
                                    const std::string& text,
                                    int cx, int y) {
    detail::centerText(r, cjk, ascii, color, text, cx, y);
}

/** 圆形轮播菜单主渲染入口。size = 画布边长 (圆屏); snap 为菜单快照。 */
inline void renderRoundMenu(SDL_Renderer* r, int size,
                            const MenuSnapshot& snap, TTF_Font* font,
                            TTF_Font* ascii_font = nullptr) {
    const int cx = size / 2, cy = size / 2;
    const int ring_r = size / 2 - 6;
    const int safe_r = static_cast<int>(size / 2 * 0.80);

    detail::setColor(r, kMenuBackdrop);
    SDL_RenderClear(r);

    // 外圆细边框 (连续)
    detail::setColor(r, kMenuPrimary);
    detail::ringCircle(r, cx, cy, ring_r);
    detail::ringCircle(r, cx, cy, ring_r - 1);

    const std::string title = snap.title.empty() ? "功能" : snap.title;
    detail::centerText(r, font, ascii_font, kMenuSecondary, title, cx,
                       cy - safe_r + 14);

    // 上/下小箭头
    detail::setColor(r, kMenuSecondary);
    detail::arrow(r, cx, cy - safe_r + 46, -1);
    detail::arrow(r, cx, cy + safe_r - 40, +1);

    const std::vector<std::string>& items = snap.items;
    const int n = static_cast<int>(items.size());
    const int sel = snap.selected_index;

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
        detail::centerText(r, font, ascii_font, kMenuPrimary, v, cx, cy - 14);
    } else {
        // 纵向轮播: 上一项 / 当前项 / 下一项
        const int prev = (sel + n - 1) % n;
        const int next = (sel + 1) % n;
        const int row_h = 56;
        detail::centerText(r, font, ascii_font, kMenuSecondary, items[prev], cx,
                           cy - row_h - 12);
        if (n > 1)
            detail::centerText(r, font, ascii_font, kMenuSecondary, items[next], cx,
                               cy + row_h + 4);
        // 当前项: 主蓝大号 + 细圆角空心框 (框内含文字, 空心不填充)
        detail::centerText(r, font, ascii_font, kMenuPrimary, items[sel], cx, cy - 10);
        detail::setColor(r, kMenuPrimary);
        const int fw = (std::min)(safe_r * 2 - 24, 260);
        const int fh = 44;
        detail::roundedFrame(r, cx - fw / 2, cy - fh / 2 - 8,
                             cx + fw / 2, cy + fh / 2 - 8, 12);
        detail::roundedFrame(r, cx - fw / 2 + 1, cy - fh / 2 - 7,
                             cx + fw / 2 - 1, cy + fh / 2 - 9, 12);
    }

    // 便签预览 (阅读时): 安全区内换行 + 页码
    if (!snap.preview_text.empty()) {
        auto lines = wrapText(snap.preview_text, 14, 3);
        int y = cy - 10;
        for (const auto& ln : lines) {
            detail::centerText(r, font, ascii_font, kMenuPrimary, ln, cx, y);
            y += 28;
        }
        detail::centerText(r, font, ascii_font, kMenuSecondary,
                           std::to_string(lines.size()) + " 行预览", cx,
                           cy + safe_r - 60);
    }
}

}  // namespace holopet
