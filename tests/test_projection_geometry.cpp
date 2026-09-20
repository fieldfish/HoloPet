/**
 * test_projection_geometry.cpp — 投影几何不变量测试 (V6-R2)
 * 圆心/半径/对称性/安全区边界/变换顺序公式。
 */

#include "display/projection_profile.hpp"
#include <cmath>
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

int main() {
    std::cout << "=== test_projection_geometry (V6-R2) ===\n\n";
    const double size = 800.0;

    // identity: 圆心不变
    {
        ProjectionProfile p;
        double cx, cy;
        transformCenter(p, size, cx, cy);
        check(near(cx, 400.0) && near(cy, 400.0), "identity 圆心 = (400,400)");
    }

    // 圆心变换与逐点变换一致 (公式自洽)
    {
        ProjectionProfile p; p.rotate_deg = 90; p.scale = 1.2;
        p.offset_x = 30; p.offset_y = -20;
        double cx, cy, px, py;
        transformCenter(p, size, cx, cy);
        applyProfile(400.0, 400.0, size, p, px, py);
        check(near(cx, px) && near(cy, py), "transformCenter == applyProfile(中心)");
    }

    // 旋转 180: 点对称
    {
        ProjectionProfile p; p.rotate_deg = 180;
        double x0, y0, x1, y1;
        applyProfile(100.0, 200.0, size, p, x0, y0);
        applyProfile(700.0, 600.0, size, p, x1, y1);
        check(near(x0, 700.0) && near(y0, 600.0), "180°: (100,200)→(700,600)");
        check(near(x1, 100.0) && near(y1, 200.0), "180°: (700,600)→(100,200)");
    }

    // 半径: scale 只缩放半径, 不移动中心
    {
        ProjectionProfile p; p.scale = 1.5;
        double cx, cy;
        transformCenter(p, size, cx, cy);
        check(near(cx, 400.0) && near(cy, 400.0), "scale 不移动中心");
        // 半径为 R 的圆经变换后仍为圆 (等距采样点到中心距离一致)
        double xa, ya, xb, yb;
        applyProfile(400.0 + 100.0, 400.0, size, p, xa, ya);
        applyProfile(400.0, 400.0 + 100.0, size, p, xb, yb);
        double ra = std::hypot(xa - cx, ya - cy);
        double rb = std::hypot(xb - cx, yb - cy);
        check(near(ra, 150.0) && near(rb, 150.0), "半径 × scale 各向同性");
    }

    // flip: 水平翻转对称
    {
        ProjectionProfile p; p.flip_h = true;
        double xa, ya, xb, yb;
        applyProfile(200.0, 300.0, size, p, xa, ya);
        applyProfile(600.0, 300.0, size, p, xb, yb);
        check(near(xa, 600.0) && near(xb, 200.0) && near(ya, 300.0) && near(yb, 300.0),
              "flip_h 镜像对称");
    }

    // 安全区: 默认 (400-40=360); 缩放后边界同步
    {
        ProjectionProfile p;
        check(near(p.safeRadius(400.0), 360.0), "安全半径 360");
        p.scale = 1.25;
        check(near(p.safeRadius(400.0) * p.scale, 450.0), "安全区随 scale 缩放");
    }

    // 变换顺序公式: flip → rotate → scale → offset 的复合效果
    {
        ProjectionProfile p;
        p.flip_h = true; p.rotate_deg = 90; p.scale = 2.0; p.offset_x = 50;
        // 手算: (x,y) 中心化 (dx,dy) → flip_h: (-dx,dy) → rotate90: (-dy,-dx)
        //       → scale2: (-2dy,-2dx) → offset: (+50+cx, +cy)
        double dx = 200.0 - 400.0, dy = 300.0 - 400.0;
        double expect_x = -2.0 * dy + 400.0 + 50.0;
        double expect_y = -2.0 * dx + 400.0;
        double ox, oy;
        applyProfile(200.0, 300.0, size, p, ox, oy);
        check(near(ox, expect_x) && near(oy, expect_y),
              "变换顺序 flip→rotate→scale→offset 公式一致");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
