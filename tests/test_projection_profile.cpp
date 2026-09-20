/**
 * test_projection_profile.cpp — V6 光学校准参数测试
 */

#include "display/projection_profile.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_projection_profile (V6) ===\n\n";

    auto p = ProjectionProfile::identity();
    check(p.valid(), "identity 有效");
    check(p.safeRadius() == 360.0, "安全半径 400-40=360");
    check(!p.perspective_enabled, "默认无透视");

    // 旋转角校验
    for (int deg : {0, 90, 180, 270}) {
        auto q = ProjectionProfile::identity();
        q.rotate_deg = deg;
        check(q.valid(), "旋转 " + std::to_string(deg) + "° 有效");
    }
    {
        auto q = ProjectionProfile::identity();
        q.rotate_deg = 45;
        check(!q.valid(), "旋转 45° 无效");
    }

    // 范围校验
    {
        auto q = ProjectionProfile::identity(); q.scale = 3.0;
        check(!q.valid(), "scale 3.0 无效");
        q = ProjectionProfile::identity(); q.scale = 0.75;
        check(q.valid(), "scale 0.75 有效");
    }
    {
        auto q = ProjectionProfile::identity(); q.brightness = 0.1;
        check(!q.valid(), "brightness 0.1 无效");
    }
    {
        auto q = ProjectionProfile::identity(); q.black_level = 100;
        check(!q.valid(), "black_level 100 无效");
    }

    // 透视四点
    {
        auto q = ProjectionProfile::identity();
        q.perspective_enabled = true;
        check(q.valid(), "默认四点透视有效");
        q.quad_tl.x = 5.0;
        check(!q.valid(), "四点超范围无效");
    }

    // 异常
    {
        ProjectionProfile q;
        q.rotate_deg = 45;
        bool threw = false;
        try { q.validate(); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "validate() 抛 invalid_argument");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
