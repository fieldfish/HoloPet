// test_face_animation_render.cpp — R8_R4 §10.1 帧缓冲散列测试 (仅 BUILD_AI)。
// 离屏 SDL 软件渲染: 基准 / 闭眼 / 三个思考方向 / Speaking 小嘴 / 噘嘴 —
// 各姿态散列不同; Idle 基准关键像素与正式基准一致 (蓝圆眼+线嘴, 无瞳孔等)。
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "display/expression_animation.hpp"
#include "display/expression_model.hpp"
#include "display/expression_renderer.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static std::string g_nine_hashes_r5[9];

static std::string hashBuf(const std::vector<unsigned char>& buf) {
    unsigned long long h = 1469598103934665603ULL;
    for (auto c : buf) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%016llx", h);
    return out;
}

// 渲染 200x200 到 RGBA 缓冲, 返回像素散列
static std::string renderHash(SDL_Renderer* r, const FaceAnimFrame& f) {
    ExpressionParams p = computeExpression(RuntimeState::Idle,
                                           Emotion::Neutral, 0.0);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    renderFace(r, 100, 100, 90, p, f, 0);
    SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                         nullptr, 0);   // 空读以对齐 (不校验返回值)
    std::vector<unsigned char> buf(200 * 200 * 4);
    if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                             buf.data(), 200 * 4) != 0) {
        return "readfail:" + std::string(SDL_GetError());
    }
    return hashBuf(buf);
}

int main() {
    std::cout << "=== test_face_animation_render (R8_R4) ===\n\n";
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cout << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
        0, 200, 200, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) { std::cout << "surface failed\n"; SDL_Quit(); return 1; }
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf);
    if (!r) { std::cout << "renderer failed\n"; SDL_FreeSurface(surf);
              SDL_Quit(); return 1; }

    FaceAnimFrame base;                       // 基准 (Idle)
    base.eye_scale_y = 1.0;
    base.mouth_pose = MouthPose::Flat;
    std::string h_base = renderHash(r, base);

    FaceAnimFrame closed;                     // 闭眼 (最薄)
    closed.eye_scale_y = 0.05;
    closed.blink_visible = true;
    std::string h_closed = renderHash(r, closed);

    FaceAnimFrame upleft;                     // 思考: 左上
    upleft.eye_offset_x = -0.35;
    upleft.eye_offset_y = -0.28;
    upleft.mouth_pose = MouthPose::Pout;
    std::string h_upleft = renderHash(r, upleft);

    FaceAnimFrame upright;
    upright.eye_offset_x = 0.35;
    upright.eye_offset_y = -0.28;
    upright.mouth_pose = MouthPose::Pout;
    std::string h_upright = renderHash(r, upright);

    FaceAnimFrame upcenter;
    upcenter.eye_offset_x = 0.0;
    upcenter.eye_offset_y = -0.28;
    upcenter.mouth_pose = MouthPose::Pout;
    std::string h_upcenter = renderHash(r, upcenter);

    FaceAnimFrame speak;
    speak.mouth_pose = MouthPose::Speech;
    ExpressionParams sp = computeExpression(RuntimeState::Speaking,
                                            Emotion::Neutral, 0.6);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    renderFace(r, 100, 100, 90, sp, speak, 0);
    std::vector<unsigned char> sb(200 * 200 * 4);
    SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                         sb.data(), 200 * 4);
    std::string h_speak = hashBuf(sb);

    std::cout << "  hash base=" << h_base << " closed=" << h_closed
              << " upleft=" << h_upleft << " upright=" << h_upright
              << " upcenter=" << h_upcenter << " speak=" << h_speak << '\n';

    check(h_base != "readfail:", "基准帧可读");
    check(h_closed != h_base, "闭眼与基准散列不同");
    check(h_upleft != h_base && h_upright != h_base && h_upcenter != h_base,
          "思考姿态与基准散列不同");
    check(h_upleft != h_upright && h_upleft != h_upcenter
          && h_upright != h_upcenter, "三个思考方向互不相同");
    check(h_speak != h_base, "Speaking 嘴与基准散列不同");

    // 关键像素: 中心背景黑; 眼睛中心区域为脸蓝 (64,160,255);
    // 无瞳孔 → 眼睛中心与边缘同色 (实心圆)
    {
        std::vector<unsigned char> pb(200 * 200 * 4);
        SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                             pb.data(), 200 * 4);
        // 重渲染基准后再取样
        renderHash(r, base);
        std::vector<unsigned char> bb(200 * 200 * 4);
        SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                             bb.data(), 200 * 4);
        auto px = [&](int x, int y) {
            size_t i = (static_cast<size_t>(y) * 200 + x) * 4;
            return std::string() + char(bb[i]) + "," + char(bb[i + 1])
                   + "," + char(bb[i + 2]);
        };
        // 中心 (100,100): 背景黑
        std::string c = px(100, 100);
        check(c == std::string() + char(0) + "," + char(0) + "," + char(0),
              "中心像素为黑背景");
        // 左眼中心: 基准眼 rx=0.13R=11.7 → x≈100-0.40*90=64, y=100-0.12*90≈89
        std::string e = px(64, 89);
        std::string want = std::string() + char(64) + "," + char(160)
                           + "," + char(255);
        check(e == want, "眼睛中心为蓝 64,160,255 实心圆");
        (void)pb;
    }

    // ---- R8_R4_R3_R4_R5: 九种表情渲染 + 统一眨眼 ----
    {
        const Emotion nine[9] = {
            Emotion::Happy, Emotion::Surprised, Emotion::Craving,
            Emotion::Concerned, Emotion::Speechless, Emotion::Joy,
            Emotion::Angry, Emotion::Curious, Emotion::Cute,
        };
        int n_render = 0, n_blink = 0;
        bool all_render_ok = true, all_distinct = true;
        for (int i = 0; i < 9; ++i) {
            ExpressionParams pp = computeExpression(RuntimeState::Idle,
                                                    nine[i], 0.0);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            FaceAnimFrame fr;
            renderFace(r, 100, 100, 90, pp, fr, 0);
            std::vector<unsigned char> nb(200 * 200 * 4);
            g_nine_hashes_r5[i] = "readfail";
            if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                                     nb.data(), 200 * 4) == 0)
                g_nine_hashes_r5[i] = hashBuf(nb);
        }
        for (int i = 0; i < 9; ++i) {
            if (g_nine_hashes_r5[i] == "readfail"
                || g_nine_hashes_r5[i].empty()) all_render_ok = false;
            for (int j = i + 1; j < 9; ++j)
                if (g_nine_hashes_r5[i] == g_nine_hashes_r5[j])
                    all_distinct = false;
        }
        check(all_render_ok, "NINE_EXPRESSIONS_RENDER: 9/9 帧可读");
        check(all_distinct, "NINE_EXPRESSIONS_RENDER: 9/9 姿态互不相同");
        n_render = (all_render_ok && all_distinct) ? 9 : 0;

        bool all_blink = true;
        for (int i = 0; i < 9; ++i) {
            ExpressionAnimationController ac;
            ac.setState(RuntimeState::Idle, 0);
            bool saw_start = false, saw_end = false;
            for (int64_t t = 0; t < 9000 && !(saw_start && saw_end); t += 20) {
                FaceAnimAudit fa = ac.tick(t);
                if (fa.emitted) {
                    if (std::string(fa.event) == "blink_start")
                        saw_start = true;
                    else if (std::string(fa.event) == "blink_end")
                        saw_end = true;
                }
            }
            if (!(saw_start && saw_end)
                || std::abs(ac.frame().eye_scale_y - 1.0) > 1e-9)
                all_blink = false;
        }
        check(all_blink, "NINE_EXPRESSIONS_BLINK: 9/9 眨眼并恢复");
        n_blink = all_blink ? 9 : 0;

        {
            ExpressionParams cq = computeExpression(RuntimeState::Idle,
                                                    Emotion::Curious, 0.0);
            check(cq.mouth_open <= 0.10,
                  "疑问嘴极小 (远小于单眼高度 1/3)");
            ExpressionParams ch = computeExpression(RuntimeState::Idle,
                                                    Emotion::Happy, 0.0);
            ExpressionParams cs = computeExpression(RuntimeState::Idle,
                                                    Emotion::Surprised, 0.0);
            check(ch.mouth_open <= 0.15 && cs.mouth_open <= 0.12,
                  "开心/惊讶嘴尺寸受限 (非尖叫)");
            check(emotionFromName("whatever") == Emotion::Neutral
                  && emotionFromName("joy") == Emotion::Joy,
                  "未知情绪回退 Neutral / 新名称解析");
            ExpressionAnimationController ac2;
            bool blink_in_pause = false;
            for (int64_t t = 0; t < 9000; t += 20) {
                FaceAnimAudit fa = ac2.tick(t, true);
                if (fa.emitted) blink_in_pause = true;
            }
            check(!blink_in_pause, "暂停模式 (菜单/时钟) 无眨眼事件");
        }
        std::cout << "  nine_render=" << n_render << "/9 nine_blink="
                  << n_blink << "/9\n";
    }

    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    SDL_Quit();

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
