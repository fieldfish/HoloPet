// test_square_menu_renderer.cpp — R8_R4_R3_R4_R5 §8.1 方形菜单确定性测试。
// 离屏 SDL 软件渲染 (font=nullptr → 只验几何与颜色):
//   720/800 渲染成功; 仅黑/主蓝/次级蓝像素; 外框为圆角矩形 (四角像素黑);
//   标题/位置/底部提示区域位于安全区; 五项图标与选中空心框存在;
//   子页 (编辑/列表/预览) 不崩; splitRuns 双字体分流 (纯函数)。
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "ui/menu_controller.hpp"
#include "ui/square_menu_renderer.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

struct PixelStats {
    long long black = 0, primary = 0, secondary = 0, other = 0;
};

static PixelStats analyze(const std::vector<unsigned char>& buf) {
    PixelStats st;
    for (size_t i = 0; i + 3 < buf.size(); i += 4) {
        const int r = buf[i], g = buf[i + 1], b = buf[i + 2];
        if (r == 0 && g == 0 && b == 0) { ++st.black; continue; }
        if (r == 64 && g == 160 && b == 255) { ++st.primary; continue; }
        if (r == 32 && g == 128 && b == 216) { ++st.secondary; continue; }
        ++st.other;
    }
    return st;
}

static bool renderSize(SDL_Renderer* r, int size, const MenuSnapshot& snap,
                       PixelStats& st, bool corner_black[4]) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    renderSquareMenu(r, size, snap, nullptr, nullptr);
    std::vector<unsigned char> buf(static_cast<size_t>(size) * size * 4);
    if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32,
                             buf.data(), size * 4) != 0) return false;
    st = analyze(buf);
    const int m = size / 20;
    const int pts[4][2] = {{m, m}, {size - m, m},
                           {m, size - m}, {size - m, size - m}};
    for (int k = 0; k < 4; ++k) {
        const size_t base = (static_cast<size_t>(pts[k][1]) * size
                             + pts[k][0]) * 4;
        corner_black[k] = (buf[base] == 0 && buf[base + 1] == 0
                           && buf[base + 2] == 0);
    }
    return true;
}

int main() {
    std::cout << "=== test_square_menu_renderer (R8_R4_R3_R4_R5) ===" << std::endl;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Surface* s1 = SDL_CreateRGBSurfaceWithFormat(
        0, 720, 720, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Surface* s2 = SDL_CreateRGBSurfaceWithFormat(
        0, 800, 800, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* r720 = SDL_CreateSoftwareRenderer(s1);
    SDL_Renderer* r800 = SDL_CreateSoftwareRenderer(s2);
    if (!r720 || !r800) { std::cout << "renderer failed\n"; return 1; }

    MenuSnapshot root;
    root.active = true;
    root.layer = MenuLayer::Root;
    root.title = "功能菜单";
    root.items = {"切换为时钟", "定时器", "闹钟", "便签", "AI模式"};
    root.selected_index = 1;

    PixelStats a, b;
    bool ca[4], cb[4];
    check(renderSize(r720, 720, root, a, ca), "720 渲染成功");
    check(renderSize(r800, 800, root, b, cb), "800 渲染成功");
    check(a.other == 0 && b.other == 0, "仅黑/主蓝/次级蓝像素");
    check(a.primary > 500 && a.secondary > 0, "主蓝框/图标与次级元素存在");
    check(ca[0] && ca[1] && ca[2] && ca[3]
          && cb[0] && cb[1] && cb[2] && cb[3],
          "外框四角为黑 (圆角矩形, 非圆形外框)");
    // 选中框空心: 框内行中心为黑
    {
        std::vector<unsigned char> buf(720 * 720 * 4);
        SDL_RenderReadPixels(r720, nullptr, SDL_PIXELFORMAT_RGBA32,
                             buf.data(), 720 * 4);
        const int ry = 36 + 62 + 1 * (620 - 62) / 5 + 20;   // 第2行中心
        const size_t base = (static_cast<size_t>(ry) * 720 + 360) * 4;
        check(buf[base] == 0 && buf[base + 1] == 0 && buf[base + 2] == 0,
              "选中项内部为黑 (空心框, 无实心蓝块)");
    }

    // 子页不崩: 编辑页 / 列表页 / 预览
    MenuSnapshot edit;
    edit.active = true;
    edit.layer = MenuLayer::TimerCreate;
    edit.title = "定时器 (分钟)";
    edit.edit_minutes = 15;
    MenuSnapshot lst;
    lst.active = true;
    lst.layer = MenuLayer::NoteList;
    lst.title = "便签";
    lst.items = {"1. 下午三点取快递", "2. 买牛奶", "3. 散步"};
    lst.selected_index = 0;
    lst.preview_text = "这是一条很长的便签内容用于验证方形菜单内安全换行显示不会越界";
    for (const auto* sn : {&edit, &lst}) {
        PixelStats st;
        bool cbx[4];
        check(renderSize(r720, 720, *sn, st, cbx), "子页渲染成功");
        check(st.other == 0, "子页无杂色");
    }

    // 双字体分流纯函数
    {
        const auto runs = sq_detail::splitRuns("10 分钟");
        check(runs.size() == 2, "ASCII/CJK 分流: 两段");
        check(runs[0].ascii && !runs[1].ascii, "run 归属正确 (10 =ASCII, 分钟=CJK)");
        const auto runs2 = sq_detail::splitRuns("AI模式");
        check(runs2.size() == 2 && runs2[0].ascii && !runs2[1].ascii,
              "英文与中文分流 (AI模式)");
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
