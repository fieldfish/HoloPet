#pragma once
/**
 * expression_model.hpp — 表情模型 (纯 C++, 无 SDL 可单测)  V6
 *
 * 状态与情绪分离: ExpressionParams = 情绪基调 × 运行状态叠加 (Speaking Happy
 * 与 Speaking Concerned 同时成立)。所有参数限定 [0,1] 或 [-1,1]。
 */

#include "agent/agent_event.hpp"

namespace holopet {

/** 表情参数 (由渲染器消费, 全部为归一化值) */
struct ExpressionParams {
    double eye_open    = 1.0;   // 0..1 眼睛睁开度
    double eye_height  = 1.0;   // 0.5..1.5 眼睛高度系数
    double brow_raise  = 0.0;   // 0..1 眉毛抬起
    double brow_tilt   = 0.0;   // -1..1 眉毛倾斜 (concerned = +)
    double mouth_curve = 0.0;   // -1..1 嘴角 (下..上)
    double mouth_open  = 0.0;   // 0..1 张嘴 (Speaking 时随电平)
    double ear_perk    = 0.0;   // 0..1
    double sway_amp    = 0.0;   // R8_R4 §3.3: Idle 禁连续摇摆
    double blink_rate  = 12.0;  // 眨眼次数/分钟
    // R8_R4_R3_R4_R5: 嘴型风格与单眼眯笑 (九种表情渲染)
    int    mouth_style = 0;     // 0平线 1笑弧 2小椭圆 3波浪(馋) 4短线(无语)
    bool   eye_squint_right = false;  // 喜悦: 右眼眯笑
    bool   question_mark = false;     // 疑问: 右上角小问号
};

namespace detail {
    inline double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
    inline double clamp01(double v) { return clamp(v, 0.0, 1.0); }
}

/**
 * 计算表情参数。
 * @param state   运行状态
 * @param emotion 情绪 (独立)
 * @param level   TTS 电平 0..1 (Speaking 时驱动嘴型)
 */
inline ExpressionParams computeExpression(RuntimeState state,
                                          Emotion emotion,
                                          double level = 0.0) {
    // 情绪基调表
    struct Base { double eye, h, brow, tilt, curve, open, ear, sway, blink; };
    static constexpr Base kBase[] = {
        /* Neutral   */ {1.00, 1.00, 0.00, 0.0, 0.00, 0.00, 0.00, 0.00, 12.0},
        /* Happy     */ {1.00, 1.05, 0.30, 0.0, 0.70, 0.10, 0.00, 0.00, 15.0},
        /* Curious   */ {1.00, 1.10, 0.70, -0.2, 0.15, 0.05, 0.00, 0.00, 14.0},
        /* Surprised */ {1.00, 1.40, 0.80, 0.0, 0.10, 0.55, 0.00, 0.00, 18.0},
        /* Sad       */ {0.80, 0.90, 0.10, 0.2, -0.60, 0.05, 0.00, 0.00, 9.0},
        /* Sleepy    */ {0.30, 0.70, 0.05, 0.0, 0.00, 0.15, 0.00, 0.00, 5.0},
        /* Concerned */ {0.90, 0.95, 0.55, 0.7, -0.20, 0.05, 0.00, 0.00, 11.0},
        /* Craving(馋) */ {1.00, 1.00, 0.00, 0.0, 0.30, 0.00, 0.00, 0.00, 12.0},
        /* Speechless(无语) */ {0.95, 1.00, 0.00, 0.0, 0.00, 0.00, 0.00, 0.00, 11.0},
        /* Joy(喜悦)     */ {1.00, 1.02, 0.00, 0.0, 0.60, 0.08, 0.00, 0.00, 14.0},
        /* Angry(生气)   */ {0.85, 0.80, 0.00, 0.0, -0.30, 0.00, 0.00, 0.00, 12.0},
        /* Cute(可爱)    */ {1.00, 1.08, 0.00, 0.0, 0.40, 0.05, 0.00, 0.00, 13.0},
    };

    const Base& b = kBase[static_cast<int>(emotion)];
    ExpressionParams p;
    p.eye_open    = detail::clamp01(b.eye);
    p.eye_height  = detail::clamp(b.h, 0.5, 1.5);
    p.brow_raise  = detail::clamp01(b.brow);
    p.brow_tilt   = detail::clamp(b.tilt, -1.0, 1.0);
    p.mouth_curve = detail::clamp(b.curve, -1.0, 1.0);
    p.mouth_open  = detail::clamp01(b.open);
    p.ear_perk    = detail::clamp01(b.ear);
    p.sway_amp    = detail::clamp(b.sway, 0.0, 0.3);
    p.blink_rate  = detail::clamp(b.blink, 2.0, 30.0);

    // R8_R4_R3_R4_R5: 九表情嘴型/单眼细则 (v3 参考板约束)
    switch (emotion) {
    case Emotion::Happy:
        p.mouth_style = 1;   // 小弧线笑嘴
        break;
    case Emotion::Surprised:
        p.mouth_style = 2;   // 极小椭圆口 (非尖叫)
        p.mouth_open = 0.08;
        break;
    case Emotion::Curious:
        p.mouth_style = 2;   // 疑问小嘴 ≤ 单眼高度 1/3
        p.mouth_open = 0.05;
        p.question_mark = true;   // 右上角小问号
        break;
    case Emotion::Craving:
        p.mouth_style = 3;   // 简洁波浪线嘴 (无彩色舌头)
        break;
    case Emotion::Speechless:
        p.mouth_style = 4;   // 简洁直线/短线
        break;
    case Emotion::Joy:
        p.mouth_style = 1;
        p.eye_squint_right = true;   // 右眼眯笑 (完整眨眼时两眼同时闭合)
        break;
    case Emotion::Angry:
        p.mouth_style = 0;   // 眼形+小嘴表达 (无眉毛)
        break;
    case Emotion::Cute:
        p.mouth_style = 1;   // 同色简约 (无腮红/心形/轮廓)
        break;
    default:
        break;
    }

    // 运行状态叠加 (状态 ≠ 情绪, 只在表情层叠加)
    switch (state) {
    case RuntimeState::Listening:
        p.mouth_open = detail::clamp01(p.mouth_open + 0.35);
        p.brow_raise = detail::clamp01(p.brow_raise + 0.15);
        break;
    case RuntimeState::Thinking:
        p.eye_open   = detail::clamp01(p.eye_open - 0.15);

        break;
    case RuntimeState::Speaking:
        p.mouth_open = detail::clamp01(0.15 + 0.85 * detail::clamp01(level));
        break;
    case RuntimeState::Error:
        p.mouth_curve = detail::clamp(p.mouth_curve - 0.3, -1.0, 1.0);
        break;
    case RuntimeState::Offline:
        p.eye_open    = detail::clamp01(p.eye_open * 0.5);
        p.mouth_curve = detail::clamp(p.mouth_curve - 0.2, -1.0, 1.0);
        break;
    case RuntimeState::Idle:
    default:
        break;
    }
    return p;
}


} // namespace holopet
