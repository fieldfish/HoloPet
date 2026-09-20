/**
 * test_expression_model.cpp — V6 表情模型 (状态×情绪正交) 测试
 */

#include "display/expression_model.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static bool inRange(const ExpressionParams& p) {
    return p.eye_open >= 0.0 && p.eye_open <= 1.0 &&
           p.eye_height >= 0.5 && p.eye_height <= 1.5 &&
           p.brow_raise >= 0.0 && p.brow_raise <= 1.0 &&
           p.brow_tilt >= -1.0 && p.brow_tilt <= 1.0 &&
           p.mouth_curve >= -1.0 && p.mouth_curve <= 1.0 &&
           p.mouth_open >= 0.0 && p.mouth_open <= 1.0 &&
           p.ear_perk >= 0.0 && p.ear_perk <= 1.0 &&
           p.sway_amp >= 0.0 && p.sway_amp <= 0.3 &&
           p.blink_rate >= 2.0 && p.blink_rate <= 30.0;
}

int main() {
    std::cout << "=== test_expression_model (V6) ===\n\n";

    // 7 情绪 × 6 状态全组合范围合法
    bool all_ok = true;
    for (int e = 0; e < 7; ++e) {
        for (int s = 0; s < 6; ++s) {
            auto p = computeExpression(static_cast<RuntimeState>(s),
                                       static_cast<Emotion>(e), 0.5);
            if (!inRange(p)) all_ok = false;
        }
    }
    check(all_ok, "7 情绪 × 6 状态全组合参数范围合法");

    // 正交: Speaking+Happy 与 Speaking+Concerned 同时成立 (状态同, 情绪不同)
    auto ph = computeExpression(RuntimeState::Speaking, Emotion::Happy, 0.6);
    auto pc = computeExpression(RuntimeState::Speaking, Emotion::Concerned, 0.6);
    check(ph.mouth_curve > 0.0, "Speaking+Happy 嘴角上翘");
    check(pc.mouth_curve < 0.0, "Speaking+Concerned 嘴角下压");
    check(ph.mouth_curve != pc.mouth_curve, "同状态不同情绪表情不同");

    // 同一情绪下 Speaking 嘴型随电平
    auto p_low  = computeExpression(RuntimeState::Speaking, Emotion::Neutral, 0.1);
    auto p_high = computeExpression(RuntimeState::Speaking, Emotion::Neutral, 0.9);
    check(p_high.mouth_open > p_low.mouth_open, "Speaking 嘴型随电平变化");

    // 情绪基调差异 (Happy vs Sad)
    auto happy = computeExpression(RuntimeState::Idle, Emotion::Happy);
    auto sad   = computeExpression(RuntimeState::Idle, Emotion::Sad);
    check(happy.mouth_curve > sad.mouth_curve, "Happy 嘴角高于 Sad");

    // Sleepy 眼睛
    auto sleepy = computeExpression(RuntimeState::Idle, Emotion::Sleepy);
    check(sleepy.eye_open < 0.4, "Sleepy 眼睛半闭");

    // Surprised 眼睛睁大 + 张嘴
    auto surp = computeExpression(RuntimeState::Idle, Emotion::Surprised);
    check(surp.eye_height > 1.2, "Surprised 眼睛放大");
    // R8_R4_R3_R4_R5: 惊讶 = 极小椭圆口 (v3 参考板, 不得尖叫脸)
    check(surp.mouth_open <= 0.12, "Surprised 极小椭圆口 (非尖叫)");

    // Listening 叠加张嘴
    auto listen = computeExpression(RuntimeState::Listening, Emotion::Neutral);
    auto idle   = computeExpression(RuntimeState::Idle, Emotion::Neutral);
    check(listen.mouth_open > idle.mouth_open, "Listening 张嘴提示");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
