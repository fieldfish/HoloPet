/**
 * test_projection_render.cpp — 离屏渲染验收 (V6-R3, 仅 BUILD_AI 编译)
 *
 * R3 收口 (相对 R2):
 *  - 纯离屏: SDL_CreateRGBSurfaceWithFormat + SDL_CreateSoftwareRenderer,
 *    不创建 Window, 不依赖 dummy 视频驱动/事件系统 (R2 在 ctest 下
 *    0xc0000279 / 46s 挂起的根因)。
 *  - 每个 SDL 调用检查返回值并输出 SDL_GetError; 失败有限时间内非零退出。
 *  - 每个 pattern 渲染前显式清屏; 输出渲染哈希 + 关键像素 + 证据路径。
 *  - BMP 证据写入系统临时目录 (不入源码包)。
 *  - 退出按逆序销毁 Renderer → Surface, 最后 SDL_Quit; 无跨 renderer 静态纹理。
 *
 * 断言内容与 R2 一致 (未降级):
 *  - 参数极值渲染哈希必须不同
 *  - identity: 中心十字白 + 安全区轮廓绿 + 安全区不填充
 *  - 同心圆: 中心一致轮廓圆, 环间不填充
 *  - 连续 20 次渲染稳定 (R3 新增, 替代“跑一次就算过”)
 */

#include "display/expression_renderer.hpp"
#include "display/projection_profile.hpp"

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

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

/** 离屏渲染上下文: Surface + SoftwareRenderer, 全返回值检查 */
struct OffscreenRender {
    SDL_Surface* surf = nullptr;
    SDL_Renderer* rend = nullptr;
    int w = 0, h = 0;

    static OffscreenRender create(int w, int h) {
        OffscreenRender o;
        o.w = w; o.h = h;
        o.surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                SDL_PIXELFORMAT_RGBA8888);
        if (!o.surf) {
            std::cout << "  FAIL: SDL_CreateRGBSurfaceWithFormat: "
                      << SDL_GetError() << '\n';
            return o;
        }
        o.rend = SDL_CreateSoftwareRenderer(o.surf);
        if (!o.rend) {
            std::cout << "  FAIL: SDL_CreateSoftwareRenderer: "
                      << SDL_GetError() << '\n';
        }
        return o;
    }

    bool ok() const { return surf && rend; }

    /** 显式清屏 + 渲染 pattern + 读回像素 (每步检查) */
    bool renderPattern(const ProjectionProfile& prof, int pattern,
                       std::vector<unsigned char>& out_buf) {
        if (!ok()) return false;
        if (SDL_SetRenderDrawColor(rend, 0, 0, 0, 255) != 0) {
            std::cout << "  FAIL: SetDrawColor: " << SDL_GetError() << '\n';
            return false;
        }
        if (SDL_RenderClear(rend) != 0) {
            std::cout << "  FAIL: RenderClear: " << SDL_GetError() << '\n';
            return false;
        }
        renderCalibrationPattern(rend, w, prof, pattern);
        out_buf.resize(static_cast<size_t>(w) * h * 4);
        SDL_Rect rect{0, 0, w, h};
        if (SDL_RenderReadPixels(rend, &rect, SDL_PIXELFORMAT_RGBA8888,
                                 out_buf.data(), w * 4) != 0) {
            std::cout << "  FAIL: RenderReadPixels: " << SDL_GetError() << '\n';
            return false;
        }
        return true;
    }

    /** 逆序销毁: Renderer → Surface */
    void destroy() {
        if (rend) { SDL_DestroyRenderer(rend); rend = nullptr; }
        if (surf) { SDL_FreeSurface(surf); surf = nullptr; }
    }
};

static bool saveBmp(SDL_Surface* surf, const std::string& path) {
    if (!surf) return false;
    if (SDL_SaveBMP(surf, path.c_str()) != 0) {
        std::cout << "  FAIL: SDL_SaveBMP (" << path << "): "
                  << SDL_GetError() << '\n';
        return false;
    }
    return true;
}

static unsigned char pixelAt(const std::vector<unsigned char>& buf,
                             int x, int y, int c, int w = 800) {
    // SDL_PIXELFORMAT_RGBA8888 小端内存序 [A,B,G,R]
    static const int map[3] = {3, 2, 1};
    return buf[(static_cast<size_t>(y) * w + x) * 4 + map[c]];
}

struct RenderResult {
    std::vector<unsigned char> buf;
    std::string hash;
};

static bool renderProfile(OffscreenRender& o, const ProjectionProfile& prof,
                          int pattern, RenderResult& res) {
    if (!o.renderPattern(prof, pattern, res.buf)) return false;
    applyPixelProfile(res.buf.data(),
                      static_cast<size_t>(o.w) * o.h, prof);
    res.hash = hashBuf(res.buf);
    return true;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    std::cout << "=== test_projection_render (V6-R3 offscreen) ===\n\n";

    // 纯离屏: 无窗口, 只需 SDL_Init(SDL_INIT_VIDEO) 初始化子系统;
    // 软件渲染器不依赖任何视频驱动的事件/窗口机制。
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cout << "  FAIL: SDL_Init: " << SDL_GetError() << '\n';
        return 1;
    }

    const int kSize = 800;
    OffscreenRender o = OffscreenRender::create(kSize, kSize);
    if (!o.ok()) {
        o.destroy();
        SDL_Quit();
        return 1;
    }

    // 1. identity: 十字中心白 + 安全区轮廓绿 (轮廓圆, 不填充)
    {
        RenderResult res;
        if (!renderProfile(o, ProjectionProfile::identity(), 0, res)) {
            o.destroy(); SDL_Quit(); return 1;
        }
        std::cout << "  identity hash=" << res.hash << '\n';
        auto dump = [&](int x, int y) {
            std::cout << "  (" << x << "," << y << ")=R"
                      << static_cast<int>(pixelAt(res.buf, x, y, 0)) << ",G"
                      << static_cast<int>(pixelAt(res.buf, x, y, 1)) << ",B"
                      << static_cast<int>(pixelAt(res.buf, x, y, 2)) << '\n';
        };
        dump(400, 400); dump(760, 400); dump(600, 400); dump(400, 10);
        check(pixelAt(res.buf, 400, 400, 0) >= 200, "identity 中心十字为白");
        check(pixelAt(res.buf, 760, 400, 1) >= 150,
              "安全区轮廓为绿 (R360 端点)");
        check(pixelAt(res.buf, 600, 400, 1) < 100, "安全区为轮廓 (内部不填充)");
    }

    // 2. 同心圆 = 轮廓圆: scale 0.5 → 半径 160/80 轮廓存在, 中心一致
    {
        ProjectionProfile p;
        p.scale = 0.5;
        RenderResult res;
        if (!renderProfile(o, p, 1, res)) { o.destroy(); SDL_Quit(); return 1; }
        std::cout << "  circles(0.5) hash=" << res.hash << '\n';
        check(pixelAt(res.buf, 400 + 160, 400, 0) >= 100,
              "scale 0.5: R160 轮廓圆存在 (中心一致)");
        check(pixelAt(res.buf, 400 + 80, 400, 0) >= 100,
              "scale 0.5: R80 轮廓圆存在");
        check(pixelAt(res.buf, 400 + 100, 400, 0) < 100,
              "同心圆为轮廓 (环间不填充)");
    }

    // 3. 参数极值两组哈希不同 (像素管线进入渲染结果)
    {
        ProjectionProfile a;
        a.gamma = 0.6; a.brightness = 1.8; a.black_level = 30;
        ProjectionProfile b;
        b.gamma = 2.2; b.brightness = 0.4; b.black_level = 0;
        RenderResult ra, rb;
        if (!renderProfile(o, a, 2, ra) || !renderProfile(o, b, 2, rb)) {
            o.destroy(); SDL_Quit(); return 1;
        }
        std::cout << "  extreme-A hash=" << ra.hash
                  << "  extreme-B hash=" << rb.hash << '\n';
        check(ra.hash != rb.hash, "极值参数渲染哈希不同");
    }

    // 4. 连续 20 次渲染稳定 (R3 新增: 同 profile 重复渲染哈希一致)
    {
        ProjectionProfile p;
        p.rotate_deg = 90; p.scale = 1.2;
        std::string first;
        bool stable = true;
        for (int i = 0; i < 20; ++i) {
            RenderResult res;
            if (!renderProfile(o, p, 0, res)) {
                stable = false; break;
            }
            if (i == 0) first = res.hash;
            else if (res.hash != first) { stable = false; break; }
        }
        std::cout << "  20x render hash=" << first << '\n';
        check(stable, "连续 20 次渲染哈希稳定一致");
    }

    // 5. BMP 证据 → 临时目录 (不入源码包; 独立 surface, 用完即释放)
    {
        auto tmp = std::filesystem::temp_directory_path() /
                   "holopet_projection_render_evidence";
        std::error_code ec;
        std::filesystem::create_directories(tmp, ec);
        bool ok = !ec;
        if (ok) {
            OffscreenRender ev = OffscreenRender::create(kSize, kSize);
            ok = ev.ok();
            if (ok) {
                ProjectionProfile p; p.rotate_deg = 90; p.scale = 1.2;
                std::vector<unsigned char> buf;
                ok = ev.renderPattern(p, 0, buf);
                if (ok) ok = saveBmp(ev.surf,
                                     (tmp / "pattern_grid_rot90.bmp").string());
                if (ok) ok = ev.renderPattern(ProjectionProfile::identity(),
                                              1, buf);
                if (ok) ok = saveBmp(ev.surf,
                                     (tmp / "pattern_circles.bmp").string());
            }
            ev.destroy();
        } else {
            std::cout << "  FAIL: create_directories: " << ec.message() << '\n';
        }
        check(ok && std::filesystem::exists(tmp / "pattern_grid_rot90.bmp") &&
              std::filesystem::exists(tmp / "pattern_circles.bmp"),
              "BMP 证据写入临时目录");
        if (ok) std::cout << "  evidence: " << tmp.string() << '\n';
    }

    // 逆序销毁: Renderer → Surface → SDL_Quit (无静态跨 renderer 资源)
    o.destroy();
    SDL_Quit();
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
