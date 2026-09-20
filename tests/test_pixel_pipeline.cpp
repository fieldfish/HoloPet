/**
 * test_pixel_pipeline.cpp — 像素管线测试 (V6-R2)
 * brightness/gamma/black_level 两组极值输出哈希必须不同; 方向符合定义。
 */

#include "display/projection_profile.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static std::string hashOf(const std::vector<unsigned char>& buf) {
    // FNV-1a 64
    unsigned long long h = 1469598103934665603ULL;
    for (auto c : buf) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%016llx", h);
    return out;
}

/** 构造灰度渐变测试图 (256×256 RGBA) */
static std::vector<unsigned char> makeGradient() {
    std::vector<unsigned char> buf(256 * 256 * 4);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            unsigned char v = static_cast<unsigned char>((x + y) % 256);
            size_t i = (static_cast<size_t>(y) * 256 + x) * 4;
            buf[i] = buf[i + 1] = buf[i + 2] = v;
            buf[i + 3] = 255;
        }
    }
    return buf;
}

int main() {
    std::cout << "=== test_pixel_pipeline (V6-R2) ===\n\n";

    auto base = makeGradient();

    // identity: 输出 ≈ 输入 (gamma=1, brightness=1, black=0; 浮点差 ≤1)
    {
        auto buf = base;
        applyPixelProfile(buf.data(), 256 * 256, ProjectionProfile::identity());
        bool ok = true;
        for (size_t i = 0; i < buf.size(); ++i) {
            int d = static_cast<int>(buf[i]) - static_cast<int>(base[i]);
            if (d < -1 || d > 1) { ok = false; break; }
        }
        check(ok, "identity 不变 (差 ≤1)");
    }

    // 两组极值哈希不同
    {
        ProjectionProfile a; a.gamma = 0.6;  a.brightness = 1.8; a.black_level = 30;
        ProjectionProfile b; b.gamma = 2.2;  b.brightness = 0.4; b.black_level = 0;
        auto bufA = base; applyPixelProfile(bufA.data(), 256 * 256, a);
        auto bufB = base; applyPixelProfile(bufB.data(), 256 * 256, b);
        check(hashOf(bufA) != hashOf(bufB), "两组极值哈希不同");
        check(hashOf(bufA) != hashOf(base), "极值 A ≠ 原图");
        check(hashOf(bufB) != hashOf(base), "极值 B ≠ 原图");
    }

    // brightness 方向: 1.5 使全图变亮 (均值上升)
    {
        ProjectionProfile p; p.brightness = 1.5;
        auto buf = base; applyPixelProfile(buf.data(), 256 * 256, p);
        long long sum = 0;
        for (auto c : buf) sum += c;
        long long sumBase = 0;
        for (auto c : base) sumBase += c;
        check(sum > sumBase, "brightness 1.5 → 均值上升");
    }

    // black_level 方向: 抬黑位 → 全黑像素变亮
    {
        std::vector<unsigned char> black(4, 0); black[3] = 255;
        ProjectionProfile p; p.black_level = 20;
        applyPixelProfile(black.data(), 1, p);
        check(black[0] == 20 && black[1] == 20 && black[2] == 20,
              "black_level 20 → 黑像素 = 20");
    }

    // gamma < 1 提亮暗部: 输入 64 → 输出 > 64
    {
        std::vector<unsigned char> px{64, 64, 64, 255};
        ProjectionProfile p; p.gamma = 0.5;
        applyPixelProfile(px.data(), 1, p);
        check(px[0] > 64, "gamma 0.5 → 暗部提亮");
    }

    // 钳位: 极亮输入不越界
    {
        std::vector<unsigned char> px{255, 255, 255, 255};
        ProjectionProfile p; p.brightness = 2.0; p.black_level = 64;
        applyPixelProfile(px.data(), 1, p);
        check(px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255,
              "钳位 [0,255] 且 alpha 不动");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
