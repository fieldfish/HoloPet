/**
 * test_profile_json.cpp — ProjectionProfile 严格 JSON save/load (V6-R2)
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
    std::cout << "=== test_profile_json (V6-R2) ===\n\n";

    // 默认 round trip
    {
        ProjectionProfile p;
        auto s = serializeProjectionProfile(p);
        ProtocolError err;
        auto q = loadProjectionProfile(s, &err);
        check(q.has_value(), "默认 round trip 加载");
        check(q->valid() && q->scale == p.scale && q->safe_margin == p.safe_margin,
              "round trip 字段一致");
    }

    // 自定义 round trip
    {
        ProjectionProfile p;
        p.flip_h = true; p.rotate_deg = 180; p.scale = 1.25;
        p.offset_x = 12; p.offset_y = -8; p.brightness = 1.4; p.gamma = 0.8;
        p.black_level = 8; p.safe_margin = 60;
        auto q = loadProjectionProfile(serializeProjectionProfile(p));
        check(q && q->flip_h && q->rotate_deg == 180 && std::abs(q->scale - 1.25) < 1e-9 &&
              q->offset_x == 12 && q->offset_y == -8 &&
              std::abs(q->brightness - 1.4) < 1e-9 && std::abs(q->gamma - 0.8) < 1e-9 &&
              q->black_level == 8 && q->safe_margin == 60,
              "自定义 round trip 全字段一致");
    }

    // 缺失字段 → 默认值
    {
        ProtocolError err;
        auto q = loadProjectionProfile(R"({"scale":1.1})", &err);
        check(q.has_value() && std::abs(q->scale - 1.1) < 1e-9 &&
              q->rotate_deg == 0 && q->safe_margin == 40,
              "缺失字段用默认值");
    }

    // 未知字段忽略
    {
        ProtocolError err;
        auto q = loadProjectionProfile(R"({"scale":1.0,"future_x":42})", &err);
        check(q.has_value() && !err.reason.empty() == false, "未知字段忽略");
    }

    // 非法范围拒绝 + 报错
    {
        ProtocolError err;
        auto q = loadProjectionProfile(R"({"scale":9.9})", &err);
        check(!q.has_value() && !err.reason.empty(), "scale 9.9 拒绝并报错");
        err = {};
        q = loadProjectionProfile(R"({"rotate_deg":45})", &err);
        check(!q.has_value(), "rotate 45 拒绝");
    }

    // 坏 JSON
    {
        ProtocolError err;
        auto q = loadProjectionProfile("{bad", &err);
        check(!q.has_value() && !err.reason.empty(), "坏 JSON 拒绝");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
