#pragma once
/**
 * projection_profile.hpp — Pepper 幻像光学校准参数 (纯 C++, 可单测)  V6
 *
 * 800×800 圆屏 + 60° 反射片校准: 翻转/旋转/缩放/偏移/亮度/Gamma/黑位/安全区;
 * 四点透视映射接口 (不硬编码观察方向)。
 */

#include "system/mini_json.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <stdexcept>

namespace holopet {

struct ProjectionProfile {
    /** 归一化 2D 点 (-1..1, 中心原点) */
    struct Point { double x = 0.0, y = 0.0; };

    bool   flip_h = false;      // 水平翻转
    bool   flip_v = false;      // 垂直翻转
    int    rotate_deg = 0;      // 仅 0/90/180/270
    double scale = 1.0;         // 0.5..2.0
    int    offset_x = 0;        // 像素偏移
    int    offset_y = 0;
    double brightness = 1.0;    // 0.3..2.0
    double gamma = 1.0;         // 0.5..2.5
    int    black_level = 0;     // 0..64
    int    safe_margin = 40;    // 圆屏安全区 (像素, 从外缘向内)

    // 四点透视映射 (可选; 不启用时 = 单位四边形)
    bool  perspective_enabled = false;
    Point quad_tl{-1, -1}, quad_tr{1, -1}, quad_br{1, 1}, quad_bl{-1, 1};

    void validate() const {
        if (rotate_deg != 0 && rotate_deg != 90 &&
            rotate_deg != 180 && rotate_deg != 270) {
            throw std::invalid_argument("ProjectionProfile: rotate_deg must be 0/90/180/270");
        }
        if (scale < 0.5 || scale > 2.0)
            throw std::invalid_argument("ProjectionProfile: scale out of [0.5,2.0]");
        if (brightness < 0.3 || brightness > 2.0)
            throw std::invalid_argument("ProjectionProfile: brightness out of [0.3,2.0]");
        if (gamma < 0.5 || gamma > 2.5)
            throw std::invalid_argument("ProjectionProfile: gamma out of [0.5,2.5]");
        if (black_level < 0 || black_level > 64)
            throw std::invalid_argument("ProjectionProfile: black_level out of [0,64]");
        if (safe_margin < 0 || safe_margin > 200)
            throw std::invalid_argument("ProjectionProfile: safe_margin out of [0,200]");
        if (perspective_enabled) {
            auto ok = [](const Point& p) {
                return std::abs(p.x) <= 2.0 && std::abs(p.y) <= 2.0;
            };
            if (!ok(quad_tl) || !ok(quad_tr) || !ok(quad_br) || !ok(quad_bl))
                throw std::invalid_argument("ProjectionProfile: quad point out of [-2,2]");
        }
    }

    bool valid() const noexcept {
        try { validate(); return true; }
        catch (...) { return false; }
    }

    /** 屏幕像素 (800) 内安全区半径: (400 - safe_margin) */
    double safeRadius(double screen_half = 400.0) const {
        return screen_half - static_cast<double>(safe_margin);
    }

    /** 默认校准值 (identity) */
    static ProjectionProfile identity() { return ProjectionProfile{}; }
};

/**
 * 像素坐标变换 (纯函数, 可单测):
 * 屏幕中心坐标系 → flip → rotate → scale → offset (顺序固定, 公式可测)
 * @param size  屏幕边长 (800)
 * @param x,y   输入像素坐标 (左上原点)
 * @param ox,oy 输出像素坐标
 */
inline void applyProfile(double x, double y, double size,
                         const ProjectionProfile& p,
                         double& ox, double& oy) {
    double cx = size / 2.0;
    // 平移到中心
    double dx = x - cx;
    double dy = y - cx;
    // flip
    if (p.flip_h) dx = -dx;
    if (p.flip_v) dy = -dy;
    // rotate (像素坐标 y 向下, 顺时针视觉旋转 = 数学逆时针)
    switch (p.rotate_deg) {
        case 90:  { double t = dx; dx = -dy; dy = t;  break; }
        case 180: { dx = -dx; dy = -dy;               break; }
        case 270: { double t = dx; dx = dy;  dy = -t; break; }
        default: break;
    }
    // scale + offset
    dx *= p.scale;
    dy *= p.scale;
    ox = dx + cx + static_cast<double>(p.offset_x);
    oy = dy + cx + static_cast<double>(p.offset_y);
}

/** 变换后的圆心 (图案中心经同一公式, 保证同心圆/安全区中心一致) */
inline void transformCenter(const ProjectionProfile& p, double size,
                            double& cx_out, double& cy_out) {
    applyProfile(size / 2.0, size / 2.0, size, p, cx_out, cy_out);
}

/**
 * 像素管线 (R2, 纯函数, 可单测): 每像素 RGBA
 *   1) gamma:   v' = 255 * (v/255)^gamma
 *   2) brightness: v' *= brightness
 *   3) black_level: v' += black_level
 *   4) clamp [0,255] (RGB 三通道; A 不动)
 * 两组极值参数必须产生不同输出 (测试锁定)。
 */
inline void applyPixelProfile(unsigned char* rgba, size_t pixel_count,
                              const ProjectionProfile& p) {
    const double inv255 = 1.0 / 255.0;
    for (size_t i = 0; i < pixel_count; ++i) {
        unsigned char* px = rgba + i * 4;
        for (int c = 0; c < 3; ++c) {
            double v = static_cast<double>(px[c]) * inv255;
            v = std::pow(v, p.gamma);                  // 1) gamma
            v *= p.brightness;                         // 2) brightness
            v = v * 255.0 + static_cast<double>(p.black_level);   // 3) black
            if (v < 0.0) v = 0.0;
            if (v > 255.0) v = 255.0;                  // 4) clamp
            px[c] = static_cast<unsigned char>(v);
        }
    }
}

/** 严格 JSON 序列化 (与 mini_json 一致; 保存后 load 可复现) */
inline std::string serializeProjectionProfile(const ProjectionProfile& p) {
    std::string out = "{";
    auto addBool = [&](const char* k, bool v) {
        out += std::string("\"") + k + "\":" + (v ? "true" : "false") + ",";
    };
    auto addNum = [&](const char* k, double v) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "\"%s\":%.6f,", k, v);
        out += buf;
    };
    auto addInt = [&](const char* k, int v) {
        out += std::string("\"") + k + "\":" + std::to_string(v) + ",";
    };
    addBool("flip_h", p.flip_h);
    addBool("flip_v", p.flip_v);
    addInt("rotate_deg", p.rotate_deg);
    addNum("scale", p.scale);
    addInt("offset_x", p.offset_x);
    addInt("offset_y", p.offset_y);
    addNum("brightness", p.brightness);
    addNum("gamma", p.gamma);
    addInt("black_level", p.black_level);
    addInt("safe_margin", p.safe_margin);
    addBool("perspective_enabled", p.perspective_enabled);
    auto addPoint = [&](const char* k, const ProjectionProfile::Point& pt) {
        out += std::string("\"") + k + "\":[" + std::to_string(pt.x) + "," +
               std::to_string(pt.y) + "],";
    };
    addPoint("quad_tl", p.quad_tl);
    addPoint("quad_tr", p.quad_tr);
    addPoint("quad_br", p.quad_br);
    addPoint("quad_bl", p.quad_bl);
    out.pop_back();   // 去尾逗号
    out += "}";
    return out;
}

/**
 * 严格 JSON 加载: 缺失字段用默认值, 未知字段忽略, 非法范围拒绝并报错。
 */
inline std::optional<ProjectionProfile> loadProjectionProfile(
    const std::string& json_text, ProtocolError* err = nullptr) {
    auto root = parseJson(json_text, err);
    if (!root) return std::nullopt;
    if (root->type != JsonValue::Type::Object) {
        if (err) *err = ProtocolError{"profile json must be an object", 0};
        return std::nullopt;
    }
    ProjectionProfile p;
    if (auto b = root->getBool("flip_h")) p.flip_h = *b;
    if (auto b = root->getBool("flip_v")) p.flip_v = *b;
    if (auto n = root->getNumber("rotate_deg")) p.rotate_deg = static_cast<int>(*n);
    if (auto n = root->getNumber("scale")) p.scale = *n;
    if (auto n = root->getNumber("offset_x")) p.offset_x = static_cast<int>(*n);
    if (auto n = root->getNumber("offset_y")) p.offset_y = static_cast<int>(*n);
    if (auto n = root->getNumber("brightness")) p.brightness = *n;
    if (auto n = root->getNumber("gamma")) p.gamma = *n;
    if (auto n = root->getNumber("black_level")) p.black_level = static_cast<int>(*n);
    if (auto n = root->getNumber("safe_margin")) p.safe_margin = static_cast<int>(*n);
    if (auto b = root->getBool("perspective_enabled")) p.perspective_enabled = *b;
    auto readPoint = [&](const char* key, ProjectionProfile::Point& pt) {
        const JsonValue* v = root->get(key);
        if (v && v->type == JsonValue::Type::Array && v->arr.size() == 2 &&
            v->arr[0].type == JsonValue::Type::Number &&
            v->arr[1].type == JsonValue::Type::Number) {
            pt.x = v->arr[0].num;
            pt.y = v->arr[1].num;
        }
    };
    readPoint("quad_tl", p.quad_tl);
    readPoint("quad_tr", p.quad_tr);
    readPoint("quad_br", p.quad_br);
    readPoint("quad_bl", p.quad_bl);
    if (!p.valid()) {
        if (err) *err = ProtocolError{"profile value out of range", 0};
        return std::nullopt;
    }
    return p;
}

} // namespace holopet
