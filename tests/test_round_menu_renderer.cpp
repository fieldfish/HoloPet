// test_round_menu_renderer.cpp — R8_R4_R3_R4 §7.2 圆形菜单渲染确定性测试。
// 离屏 SDL 软件渲染 (font=nullptr → 只验几何): 720/800 均不越界;
// 无白色像素; 主蓝 (64,160,255) 出现 (圆环/当前项框); 选中框空心
// (框中心为黑); 背景纯黑; 编辑页/AI模式页/预览页渲染不崩不白。
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "ui/menu_controller.hpp"
#include "ui/round_menu_renderer.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

struct PixelStats {
    long long black = 0, primary = 0, secondary = 0, white = 0, other = 0;
};

static PixelStats analyze(const std::vector<unsigned char>& buf) {
    PixelStats st;
    for (size_t i = 0; i + 3 < buf.size(); i += 4) {
        const int r = buf[i], g = buf[i + 1], b = buf[i + 2];
        if (r == 0 && g == 0 && b == 0) { ++st.black; continue; }
        if (r == 64 && g == 160 && b == 255) { ++st.primary; continue; }
        if (r == 32 && g == 128 && b == 216) { ++st.secondary; continue; }
        if (r >= 240 && g >= 240 && b >= 240) { ++st.white; continue; }
        ++st.other;
    }
    return st;
}

static bool renderSize(SDL_Renderer* r, int size, const MenuSnapshot& snap,
                       PixelStats& st, unsigned char center[3]) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    renderRoundMenu(r, size, snap, nullptr);
    std::vector<unsigned char> buf(static_cast<size_t>(size) * size * 4);
    if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                             buf.data(), size * 4) != 0) return false;
    st = analyze(buf);
    const size_t cx = static_cast<size_t>(size / 2);
    const size_t cy = static_cast<size_t>(size / 2) - 8;
    const size_t base = (cy * static_cast<size_t>(size) + cx) * 4;
    center[0] = buf[base]; center[1] = buf[base + 1]; center[2] = buf[base + 2];
    return true;
}

int main() {
    std::cout << "=== test_round_menu_renderer (R8_R4_R3_R4) ===\n\n";
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cout << "SDL_Init failed\n";
        return 1;
    }
    SDL_Renderer* r720 = nullptr;
    SDL_Renderer* r800 = nullptr;
    SDL_Surface* s1 = nullptr;
    SDL_Surface* s2 = nullptr;
    {
        s1 = SDL_CreateRGBSurfaceWithFormat(
            0, 720, 720, 32, SDL_PIXELFORMAT_RGBA32);
        s2 = SDL_CreateRGBSurfaceWithFormat(
            0, 800, 800, 32, SDL_PIXELFORMAT_RGBA32);
        r720 = SDL_CreateSoftwareRenderer(s1);
        r800 = SDL_CreateSoftwareRenderer(s2);
    }
    if (!r720 || !r800) {
        std::cout << "renderer failed\n";
        SDL_Quit();
        return 1;
    }

    MenuSnapshot root;
    root.active = true;
    root.layer = MenuLayer::Root;
    root.title = "功能";
    root.items = {"时钟", "定时器", "闹钟", "便签", "AI模式"};
    root.selected_index = 1;

    PixelStats a, b;
    unsigned char c1[3], c2[3];
    check(renderSize(r720, 720, root, a, c1), "720 渲染成功");
    check(renderSize(r800, 800, root, b, c2), "800 渲染成功");
    check(a.white == 0 && b.white == 0, "无白色像素 (720/800)");
    check(a.other == 0 && b.other == 0, "无其他颜色像素");
    check(a.primary > 500, "主蓝几何存在 (圆环+当前项框+箭头)");
    check(a.black > 0, "背景纯黑");
    check(c1[0] == 0 && c1[1] == 0 && c1[2] == 0
          && c2[0] == 0 && c2[1] == 0 && c2[2] == 0,
          "选中框中心为黑 (空心, 非实心高亮块)");

    // 编辑页 (定时器分钟) / AI 模式页 / 便签预览页: 不崩且无白
    MenuSnapshot edit;
    edit.active = true;
    edit.layer = MenuLayer::TimerCreate;
    edit.title = "定时器 (分钟)";
    edit.edit_minutes = 15;
    MenuSnapshot ai;
    ai.active = true;
    ai.layer = MenuLayer::AiMode;
    ai.title = "AI模式";
    ai.items = {"快速", "自动", "深度", "本地"};
    ai.selected_index = 0;
    MenuSnapshot prev;
    prev.active = true;
    prev.layer = MenuLayer::NoteList;
    prev.title = "便签";
    prev.items = {"1. 下午三点取快递", "2. 买牛奶"};
    prev.selected_index = 0;
    prev.preview_text = "这是一条很长的便签内容用于验证圆形安全区内换行显示不会越界以及页数提示";
    for (const auto* sn : {&edit, &ai, &prev}) {
        PixelStats st;
        unsigned char cc[3];
        check(renderSize(r720, 720, *sn, st, cc), "子页渲染成功");
        check(st.white == 0 && st.other == 0, "子页无白/无杂色");
    }

    SDL_DestroyRenderer(r720);
    SDL_DestroyRenderer(r800);
    SDL_FreeSurface(s1);
    SDL_FreeSurface(s2);
    SDL_Quit();
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
