#pragma once
/**
 * expression_renderer.hpp — SDL 圆屏表情/校准渲染 (V6)
 *
 * 只使用 SDL 图元 (无来源不清的角色素材); 动画进度由单调时钟 (now_ms)
 * 驱动, 不通过累积 SDL_Delay 控制关键时序; 60 FPS 由主循环节拍。
 */

#include "display/expression_animation.hpp"
#include "display/expression_model.hpp"
#include "display/projection_profile.hpp"

#ifdef _WIN32
#define NOMINMAX   // windows.h 的 max/min 宏与 std::max 冲突
#endif
#include <SDL.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace holopet {

// R8_R3_R2: 背景纯黑; 生产表情 (眼睛/嘴巴/耳朵/轮廓) 统一蓝 RGB(64,160,255)。
inline constexpr SDL_Color kBackdrop = {0, 0, 0, 255};
inline constexpr SDL_Color kFaceColor = {64, 160, 255, 255};

/** 填充圆 (SDL 无圆 API, 多边形近似); R8_R4_R3_R2: 可指定颜色
 * (旧实现填充色硬编码 kFaceColor — 黑内盘被画成蓝盘, 时钟成纯圆)。 */
inline void renderFilledCircle(SDL_Renderer* r, int cx, int cy, int radius,
                               int segments = 48,
                               SDL_Color color = kFaceColor) {
    if (radius <= 0) return;
    SDL_FPoint pts[64];
    int n = segments < 64 ? segments : 64;
    for (int i = 0; i < n; ++i) {
        double a = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / n;
        pts[i].x = static_cast<float>(cx + radius * std::cos(a));
        pts[i].y = static_cast<float>(cy + radius * std::sin(a));
    }
    for (int i = 0; i < n; ++i) {
        SDL_RenderDrawLineF(r, cx, cy, pts[i].x, pts[i].y);
        SDL_RenderDrawLineF(r, pts[i].x, pts[i].y, pts[(i + 1) % n].x, pts[(i + 1) % n].y);
    }
    // 三角形扇填充
    for (int i = 0; i < n; ++i) {
        SDL_Vertex v[3];
        auto setv = [&](int k, SDL_FPoint p) {
            v[k].position = p;
            v[k].color = {color.r, color.g, color.b, 255};
        };
        setv(0, {static_cast<float>(cx), static_cast<float>(cy)});
        setv(1, pts[i]);
        setv(2, pts[(i + 1) % n]);
        SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
    }
}

/** 填充椭圆 (多边形近似, 支持半轴缩放) */
inline void renderFilledEllipse(SDL_Renderer* r, int cx, int cy,
                                int rx, int ry, int segments = 40) {
    if (rx <= 0 || ry <= 0) return;
    SDL_Vertex v[3];
    auto setv = [&](int k, float x, float y) {
        v[k].position = {x, y};
        v[k].color = {kFaceColor.r, kFaceColor.g, kFaceColor.b, 255};
    };
    float px = 0.0f, py = 0.0f;
    for (int i = 0; i <= segments; ++i) {
        double a = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / segments;
        float x = static_cast<float>(cx + rx * std::cos(a));
        float y = static_cast<float>(cy + ry * std::sin(a));
        if (i > 0) {
            setv(0, static_cast<float>(cx), static_cast<float>(cy));
            setv(1, px, py);
            setv(2, x, y);
            SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
        }
        px = x; py = y;
    }
}

/** 画线 (带颜色) */
inline void renderLine(SDL_Renderer* r, float x0, float y0, float x1, float y1) {
    SDL_RenderDrawLineF(r, x0, y0, x1, y1);
}

// 黑底 + 生产表情统一蓝 (R8_R3_R2): 眼睛/嘴巴/耳朵全部使用 kFaceColor。

/**
 * 表情主渲染: 黑底 + 蓝色表情。
 * R8_R3_R2: 去掉眉毛 (R1 已定); 黑底 + 生产表情统一蓝色 kFaceColor。
 * @param blink 当前帧是否眨眼 (由主循环用 blink_rate 判定)
 */
inline void renderFace(SDL_Renderer* r, int cx, int cy, int radius,
                       const ExpressionParams& p,
                       const FaceAnimFrame& anim,
                       int64_t now_ms) {
    (void)now_ms;
    SDL_SetRenderDrawColor(r, kBackdrop.r, kBackdrop.g, kBackdrop.b, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, kFaceColor.r, kFaceColor.g, kFaceColor.b, 255);

    // 摆动 (单调时钟驱动相位)
    double phase = static_cast<double>(now_ms) / 1000.0 * 2.0 * 3.14159265358979323846;
    int sway = static_cast<int>(std::sin(phase / 3.0) * p.sway_amp * radius);

    // 保持简约轮廓：不绘制耳朵、眉毛或其他顶部装饰。
    (void)0;

    // 眼睛 (R8_R4: 动画帧 — eye_scale_y 压缩 / 视线偏移 / 眨眼可见)
    const double eye_off_x = 0.40;
    const double scale_y = anim.eye_scale_y < 0.05 ? 0.05 : anim.eye_scale_y;
    const int eye_ry = (std::max)(3, static_cast<int>(
        radius * 0.17 * p.eye_height * scale_y));
    const int eye_rx = (std::max)(4, static_cast<int>(radius * 0.13));
    const int eye_open = anim.blink_visible
        ? (std::max)(1, static_cast<int>(eye_ry * 0.10))
        : (std::max)(1, static_cast<int>(eye_ry * p.eye_open));
    const int gx = static_cast<int>(anim.eye_offset_x * eye_rx);
    const int gy = static_cast<int>(anim.eye_offset_y * eye_rx);
    const int eye_y = cy - static_cast<int>(radius * 0.12) + gy;
    for (int s : {-1, 1}) {
        int ex = cx + s * static_cast<int>(radius * eye_off_x) + gx + sway;
        // R8_R4_R3_R4_R5: 喜悦右眼眯笑 (s=+1); 完整眨眼时 eye_scale_y
        // 统一压缩两眼 (blink_visible 路径不受单眼眯影响)
        int eh = eye_open;
        if (p.eye_squint_right && s == 1 && !anim.blink_visible) {
            eh = (std::max)(1, eye_open / 3);
        }
        renderFilledEllipse(r, ex, eye_y, eye_rx, eh);
    }

    // 眉毛已按用户要求移除 (R8_R3_R1): 不再绘制眼睛上方的横线

    // R8_R4_R3_R4_R5_R3: 疑问表情右上角小问号 (蓝线, 钩+点)
    if (p.question_mark) {
        const int qx = cx + static_cast<int>(radius * 0.58);
        const int qy = cy - static_cast<int>(radius * 0.62);
        const int qr = (std::max)(6, static_cast<int>(radius * 0.09));
        for (int i = 0; i < 14; ++i) {
            const double a0 = 3.14159265358979323846 * (0.85 + 0.5 * i / 14.0);
            const double a1 = 3.14159265358979323846 * (0.85 + 0.5 * (i + 1) / 14.0);
            SDL_RenderDrawLineF(r,
                static_cast<float>(qx + qr * std::cos(a0)),
                static_cast<float>(qy + qr * std::sin(a0)),
                static_cast<float>(qx + qr * std::cos(a1)),
                static_cast<float>(qy + qr * std::sin(a1)));
        }
        SDL_RenderDrawLineF(r, static_cast<float>(qx + qr * 0.75),
                            static_cast<float>(qy + qr * 0.15),
                            static_cast<float>(qx + qr * 0.75),
                            static_cast<float>(qy + qr * 0.55));
        SDL_RenderDrawLineF(r, static_cast<float>(qx + qr * 0.75),
                            static_cast<float>(qy + qr * 0.55),
                            static_cast<float>(qx + qr * 0.35),
                            static_cast<float>(qy + qr * 0.75));
        SDL_RenderDrawLineF(r, static_cast<float>(qx + qr * 0.35),
                            static_cast<float>(qy + qr * 0.75),
                            static_cast<float>(qx + qr * 0.35),
                            static_cast<float>(qy + qr * 0.9));
        SDL_RenderDrawLineF(r, static_cast<float>(qx + qr * 0.35),
                            static_cast<float>(qy + qr * 1.06),
                            static_cast<float>(qx + qr * 0.5),
                            static_cast<float>(qy + qr * 1.06));
    }

    // 嘴 (R8_R4: Pout=极浅三段小噘嘴 宽45~60% 高≤12%眼半径; Speech 沿用 RMS)
    const int mouth_y = cy + static_cast<int>(radius * 0.34);
    const int mouth_w = static_cast<int>(radius * 0.30);
    if (anim.mouth_pose == MouthPose::Pout && p.mouth_open <= 0.12) {
        const int pw = static_cast<int>(mouth_w * 0.52);
        const int ph = (std::max)(1, static_cast<int>(eye_rx * 0.10));
        const int m0 = cy + static_cast<int>(radius * 0.34) - ph;
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - pw / 2 + sway), static_cast<float>(m0),
            static_cast<float>(cx - pw / 6 + sway), static_cast<float>(m0 + ph));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - pw / 6 + sway), static_cast<float>(m0 + ph),
            static_cast<float>(cx + pw / 6 + sway), static_cast<float>(m0 + ph));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx + pw / 6 + sway), static_cast<float>(m0 + ph),
            static_cast<float>(cx + pw / 2 + sway), static_cast<float>(m0));
    } else if (p.mouth_style == 2 || p.mouth_open > 0.12) {
        // 小椭圆口 (疑问/惊讶: 极小; 说话: RMS 大小)
        const double mopen = (p.mouth_style == 2)
            ? p.mouth_open : p.mouth_open;
        renderFilledEllipse(r, cx + sway, mouth_y,
                            (std::max)(3, static_cast<int>(mouth_w * 0.5
                                * (p.mouth_style == 2 ? 0.5 : 1.0))),
                            (std::max)(2, static_cast<int>(
                                radius * 0.16 * mopen)));
    } else if (p.mouth_style == 3) {
        // 馋: 简洁波浪线嘴 (三段小折线, 无彩色舌头)
        const int m0 = mouth_y;
        const int w = mouth_w / 2;
        const int h = (std::max)(2, static_cast<int>(eye_rx * 0.12));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - w + sway), static_cast<float>(m0),
            static_cast<float>(cx - w / 2 + sway), static_cast<float>(m0 + h));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - w / 2 + sway), static_cast<float>(m0 + h),
            static_cast<float>(cx + sway), static_cast<float>(m0));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx + sway), static_cast<float>(m0),
            static_cast<float>(cx + w / 2 + sway), static_cast<float>(m0 + h));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx + w / 2 + sway), static_cast<float>(m0 + h),
            static_cast<float>(cx + w + sway), static_cast<float>(m0));
    } else if (p.mouth_style == 4) {
        // 无语: 简洁短线
        const int w = mouth_w / 3;
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - w + sway), static_cast<float>(mouth_y),
            static_cast<float>(cx + w + sway), static_cast<float>(mouth_y));
    } else {
        // 闭合: 曲线折线
        const int depth = static_cast<int>(p.mouth_curve * radius * 0.06);
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx - mouth_w / 2 + sway),
            static_cast<float>(mouth_y),
            static_cast<float>(cx + sway),
            static_cast<float>(mouth_y + depth));
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx + sway),
            static_cast<float>(mouth_y + depth),
            static_cast<float>(cx + mouth_w / 2 + sway),
            static_cast<float>(mouth_y));
    }
}

/**
 * 轮廓圆 (不填充; SDL 无圆 API → 折线环)
 */
inline void renderCircleOutline(SDL_Renderer* r, int cx, int cy, int radius,
                                int segments = 64) {
    if (radius <= 0) return;
    int n = segments < 64 ? segments : 64;
    for (int i = 0; i < n; ++i) {
        double a0 = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / n;
        double a1 = 2.0 * 3.14159265358979323846 * static_cast<double>(i + 1) / n;
        SDL_RenderDrawLineF(r,
            static_cast<float>(cx + radius * std::cos(a0)),
            static_cast<float>(cy + radius * std::sin(a0)),
            static_cast<float>(cx + radius * std::cos(a1)),
            static_cast<float>(cy + radius * std::sin(a1)));
    }
}

/**
 * 校准画面 (pattern): 全部图案尊重同一 ProjectionProfile (几何变换)。
 *   0 十字+网格  1 同心圆  2 灰阶  3 边缘裁剪  4 人形轮廓示例
 * 同心圆 = 变换后中心 + radius*scale 的轮廓圆 (中心一致, 不填满);
 * 安全区 = 变换后中心的轮廓边界 (不填充)。
 */
#ifdef HOLOPET_PROJECTION
inline void renderCalibrationPattern(SDL_Renderer* r, int size,
                                     const ProjectionProfile& prof, int pattern) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    double ccx, ccy;
    transformCenter(prof, static_cast<double>(size), ccx, ccy);

    if (pattern == 0 || pattern == 3) {
        // 网格 (步进 80px; 边缘裁剪图同网格)
        SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
        for (int g = 0; g <= size; g += 80) {
            double x0, y0, x1, y1;
            applyProfile(static_cast<double>(g), 0.0, size, prof, x0, y0);
            applyProfile(static_cast<double>(g), size, size, prof, x1, y1);
            SDL_RenderDrawLineF(r, static_cast<float>(x0), static_cast<float>(y0),
                                   static_cast<float>(x1), static_cast<float>(y1));
            applyProfile(0.0, static_cast<double>(g), size, prof, x0, y0);
            applyProfile(size, static_cast<double>(g), size, prof, x1, y1);
            SDL_RenderDrawLineF(r, static_cast<float>(x0), static_cast<float>(y0),
                                   static_cast<float>(x1), static_cast<float>(y1));
        }
    }
    if (pattern == 0) {
        // 中心短十字 (R2: 不贯穿全屏, 避免污染同心圆/安全区判读)
        const double arm = 60.0;
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        double x0, y0, x1, y1;
        applyProfile(size / 2.0, size / 2.0 - arm, size, prof, x0, y0);
        applyProfile(size / 2.0, size / 2.0 + arm, size, prof, x1, y1);
        SDL_RenderDrawLineF(r, static_cast<float>(x0), static_cast<float>(y0),
                               static_cast<float>(x1), static_cast<float>(y1));
        applyProfile(size / 2.0 - arm, size / 2.0, size, prof, x0, y0);
        applyProfile(size / 2.0 + arm, size / 2.0, size, prof, x1, y1);
        SDL_RenderDrawLineF(r, static_cast<float>(x0), static_cast<float>(y0),
                               static_cast<float>(x1), static_cast<float>(y1));
    }
    if (pattern == 1) {
        // 同心圆: 轮廓圆, 中心 = 变换后中心 (不填充)
        SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
        for (int rr : {80, 160, 240, 320}) {
            renderCircleOutline(r, static_cast<int>(ccx), static_cast<int>(ccy),
                                static_cast<int>(rr * prof.scale));
        }
    }
    if (pattern == 2) {
        // 灰阶条 (8 级, 横排, 中心经变换; 亮度由像素管线统一处理)
        int w = size / 8;
        for (int i = 0; i < 8; ++i) {
            int v = static_cast<int>(255.0 * i / 7.0);
            SDL_SetRenderDrawColor(r, static_cast<unsigned char>(v),
                                      static_cast<unsigned char>(v),
                                      static_cast<unsigned char>(v), 255);
            double x0, y0, x1, y1;
            applyProfile(static_cast<double>(i * w), size / 2.0 - size / 8.0,
                         size, prof, x0, y0);
            applyProfile(static_cast<double>((i + 1) * w), size / 2.0 + size / 8.0,
                         size, prof, x1, y1);
            SDL_FRect rc{static_cast<float>(x0), static_cast<float>(y0),
                         static_cast<float>(x1 - x0), static_cast<float>(y1 - y0)};
            SDL_RenderFillRectF(r, &rc);
        }
    }
    if (pattern == 4) {
        // 人形轮廓示例: 头圆 (轮廓) + 眼 + 嘴 (供反射观察对齐)
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        renderCircleOutline(r, static_cast<int>(ccx),
                            static_cast<int>(ccy - size * 0.15 * prof.scale),
                            static_cast<int>(size * 0.22 * prof.scale));
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        renderFilledCircle(r, static_cast<int>(ccx - size * 0.08 * prof.scale),
                           static_cast<int>(ccy - size * 0.18 * prof.scale),
                           static_cast<int>(size * 0.03 * prof.scale));
        renderFilledCircle(r, static_cast<int>(ccx + size * 0.08 * prof.scale),
                           static_cast<int>(ccy - size * 0.18 * prof.scale),
                           static_cast<int>(size * 0.03 * prof.scale));
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDrawLineF(r,
            static_cast<float>(ccx - size * 0.08 * prof.scale),
            static_cast<float>(ccy - size * 0.05 * prof.scale),
            static_cast<float>(ccx + size * 0.08 * prof.scale),
            static_cast<float>(ccy - size * 0.05 * prof.scale));
    }
    // 安全区: 变换后中心的轮廓边界 (不填充, 不遮挡内容)
    SDL_SetRenderDrawColor(r, 0, 220, 0, 255);
    renderCircleOutline(r, static_cast<int>(ccx), static_cast<int>(ccy),
                        static_cast<int>(prof.safeRadius(size / 2.0) * prof.scale));
}
#endif  // HOLOPET_PROJECTION

/**
 * 帧呈现管线 (R3): 读取渲染目标 → 逐像素 brightness/gamma/black_level
 * → 上传显示。校准与生产共用同一 ProjectionProfile 像素处理。
 *
 * R3 修正: 纹理不再是无主的静态指针 — 绑定 (renderer, w, h) 三元组,
 * renderer 或尺寸变化时销毁重建; 退出前调用 presentFrameDestroy()
 * (须在 SDL_DestroyRenderer 之前) 释放。仅限 SDL 主线程调用。
 */
struct FramePipeline {
    SDL_Texture* tex = nullptr;
    SDL_Renderer* owner = nullptr;
    int w = 0, h = 0;
    std::vector<unsigned char> buf;
};

inline FramePipeline& framePipeline() {
    static FramePipeline fp;
    return fp;
}

/** 释放 presentFrame 持有的纹理; 必须在销毁对应 renderer 之前调用。 */
inline void presentFrameDestroy() {
    FramePipeline& fp = framePipeline();
    if (fp.tex) {
        if (fp.owner) SDL_DestroyTexture(fp.tex);   // owner 已销毁则无能为力
        fp.tex = nullptr;
    }
    fp.owner = nullptr;
    fp.w = fp.h = 0;
    fp.buf.clear();
    fp.buf.shrink_to_fit();
}

/** @return 成功与否 (读回失败时直接 present 原帧) */
inline bool presentFrame(SDL_Renderer* r, int w, int h,
                         const ProjectionProfile& prof) {
    FramePipeline& fp = framePipeline();
    if (fp.buf.size() != static_cast<size_t>(w) * h * 4) {
        fp.buf.resize(static_cast<size_t>(w) * h * 4);
    }
    if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA8888,
                             fp.buf.data(), w * 4) != 0) {
        SDL_RenderPresent(r);
        return false;
    }
    applyPixelProfile(fp.buf.data(), static_cast<size_t>(w) * h, prof);
    // 纹理生命周期: renderer/尺寸变化 → 销毁重建 (不再跨 renderer 复用)
    if (fp.tex && (fp.owner != r || fp.w != w || fp.h != h)) {
        SDL_DestroyTexture(fp.tex);
        fp.tex = nullptr;
    }
    if (!fp.tex) {
        fp.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!fp.tex) { SDL_RenderPresent(r); return false; }
        fp.owner = r; fp.w = w; fp.h = h;
    }
    SDL_UpdateTexture(fp.tex, nullptr, fp.buf.data(), w * 4);
    SDL_RenderClear(r);
    SDL_RenderCopy(r, fp.tex, nullptr, nullptr);
    SDL_RenderPresent(r);
    return true;
}

} // namespace holopet
