// test_expression_animation.cpp — R8_R4 表情微动画控制器确定性测试 (§10.1)。
// 固定 now_ms 与 seed: 眨眼间隔/时长/双眨、思考方向与偏移界限、姿态次数、
// 回归基准、审计低频与基准几何不变。
#include <cmath>
#include <cinttypes>
#include <iostream>
#include <string>
#include <vector>

#include "display/expression_animation.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static bool nearEq(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

int main() {
    std::cout << "=== test_expression_animation (R8_R4) ===\n\n";

    {
        // 1. Idle 基准几何不变 (95% 帧与冻结基准一致)
        ExpressionAnimationController c(42u);
        c.setState(RuntimeState::Idle, 0);
        int idle_frames = 0, deviant = 0;
        for (int64_t t = 0; t < 60000; t += 16) {
            c.tick(t);
            if (c.frame().phase == FaceAnimPhase::Idle) {
                ++idle_frames;
                if (!nearEq(c.frame().eye_scale_y, 1.0)
                    || !nearEq(c.frame().eye_offset_x, 0.0)
                    || !nearEq(c.frame().eye_offset_y, 0.0)
                    || c.frame().mouth_pose != MouthPose::Flat) ++deviant;
            }
        }
        check(idle_frames > 0, "Idle 帧样本存在");
        check(deviant * 100 <= idle_frames * 5,
              "Idle ≥95% 与基准几何一致");
    }

    {
        // 2. 眨眼间隔在 4.5~8.0s
        ExpressionAnimationController c(7u);
        c.setState(RuntimeState::Idle, 0);
        int64_t prev = 0, min_gap = INT64_MAX, max_gap = 0, blinks = 0;
        for (int64_t t = 0; t < 60000; t += 4) {
            FaceAnimAudit a = c.tick(t);
            if (a.emitted && a.event == "blink_start") {
                if (prev > 0) {
                    int64_t gap = t - prev;
                    if (gap > 1000) {  // 双眨对 (≤~400ms) 单独由测试 5 约束
                        min_gap = gap < min_gap ? gap : min_gap;
                        max_gap = gap > max_gap ? gap : max_gap;
                    }
                }
                prev = t; ++blinks;
            }
        }
        check(blinks >= 6, "60s 内眨眼 ≥6 次");
        check(min_gap >= 4500 - 4, "眨眼间隔 ≥4.5s");
        check(max_gap <= 8000 + 4, "眨眼间隔 ≤8.0s");
    }

    {
        // 3. 单次眨眼时长 ≤240ms 且完成闭合+停留
        ExpressionAnimationController c(3u);
        c.setState(RuntimeState::Idle, 0);
        int64_t start = -1;
        for (int64_t t = 0; t < 9000; t += 4) {
            if (c.tick(t).emitted) { start = t; break; }
        }
        check(start >= 0, "出现 blink_start");
        int64_t end = -1;
        for (int64_t t = start; t < start + 500; t += 4) {
            FaceAnimAudit a = c.tick(t);
            if (a.emitted && a.event == "blink_end") { end = t; break; }
        }
        check(end >= 0, "出现 blink_end");
        check(end - start <= 240, "单眨总时长 ≤240ms");
        check(end - start >= 110, "闭合+停留至少 110ms");
    }

    {
        // 4. 闭眼最薄不消失
        ExpressionAnimationController c(11u);
        c.setState(RuntimeState::Idle, 0);
        int64_t start = -1;
        for (int64_t t = 0; t < 9000; t += 4) {
            if (c.tick(t).emitted) { start = t; break; }
        }
        double min_scale = 1.0;
        for (int64_t t = start; t < start + 260; t += 4) {
            c.tick(t);
            min_scale = c.frame().eye_scale_y < min_scale
                ? c.frame().eye_scale_y : min_scale;
        }
        check(min_scale >= 0.05, "闭眼最薄 eye_scale_y ≥0.05 (不消失)");
    }

    {
        // 5. 双眨比例有界 (≤20% 采样噪声内)
        ExpressionAnimationController c(5u);
        c.setState(RuntimeState::Idle, 0);
        int64_t prev_start = -1, total = 0, doubles = 0;
        for (int64_t t = 0; t < 600000; t += 4) {
            FaceAnimAudit a = c.tick(t);
            if (a.emitted && a.event == "blink_start") {
                if (prev_start > 0 && t - prev_start <= 180 + 4
                    && t - prev_start >= 110 - 4) ++doubles;
                prev_start = t; ++total;
            }
        }
        check(total >= 70, "10 分钟采样眨眼总数足够");
        check(doubles * 100 <= total * 20, "双眨比例 ≤20%");
    }

    {
        // 6. Thinking 方向/偏移界限/姿态次数
        for (uint64_t seed : {1u, 2u, 3u}) {
            ExpressionAnimationController c(seed);
            c.setState(RuntimeState::Thinking, 100000);
            std::vector<std::string> dirs;
            bool hold_seen = false;
            for (int64_t t = 100000; t < 106000; t += 8) {
                FaceAnimAudit a = c.tick(t);
                if (a.emitted && a.event == "thinking_pose")
                    dirs.push_back(a.direction);
                if (c.frame().phase == FaceAnimPhase::ThinkingHold) {
                    hold_seen = true;
                    if (c.frame().eye_offset_x < -0.45 - 1e-6
                        || c.frame().eye_offset_x > 0.45 + 1e-6) ++g_failed;
                    if (c.frame().eye_offset_y > -0.20 + 0.07
                        || c.frame().eye_offset_y < -0.35 - 1e-6) ++g_failed;
                }
            }
            check(!dirs.empty() && hold_seen, "Thinking 产生方向并保持姿态");
            bool ok = true;
            for (const auto& d : dirs)
                ok = ok && (d == "upper_left" || d == "upper_right"
                            || d == "upper_center");
            check(ok, "方向 ∈ {upper_left, upper_right, upper_center}");
            check(c.poseChangesThisEpisode() <= 2, "每轮换向 ≤2 次");
        }
    }

    {
        // 7. 首个内容到达后 ≤180ms 回归基准
        ExpressionAnimationController c(9u);
        c.setState(RuntimeState::Thinking, 50000);
        for (int64_t t = 50000; t < 53000; t += 8) c.tick(t);
        c.setState(RuntimeState::Idle, 53000);
        int64_t restored = -1;
        for (int64_t t = 53000; t < 53400; t += 4) {
            FaceAnimAudit a = c.tick(t);
            if (a.emitted && a.event == "baseline_restored") { restored = t; break; }
        }
        check(restored >= 0, "产生 baseline_restored");
        check(restored - 53000 <= 180 + 8, "回归 ≤180ms");
        check(nearEq(c.frame().eye_offset_x, 0.0)
              && nearEq(c.frame().eye_offset_y, 0.0), "回归后偏移为 0");
        check(c.frame().mouth_pose == MouthPose::Flat, "回归后嘴型 flat");
    }

    {
        // 8. 审计低频 (60s 内事件数远小于帧数)
        ExpressionAnimationController c(13u);
        c.setState(RuntimeState::Idle, 0);
        int events = 0;
        for (int64_t t = 0; t < 60000; t += 16) {
            if (c.tick(t).emitted) ++events;
        }
        check(events > 0 && events < 60, "审计事件低频 (无逐帧刷屏)");
    }

    {
        // 9. Speaking 嘴型由状态驱动, 回到 Idle 恢复 flat
        ExpressionAnimationController c(17u);
        c.setState(RuntimeState::Speaking, 0);
        check(c.frame().mouth_pose == MouthPose::Speech,
              "Speaking → mouth_pose=speech");
        c.setState(RuntimeState::Idle, 100);
        for (int64_t t = 100; t < 400; t += 4) c.tick(t);
        check(c.frame().mouth_pose == MouthPose::Flat,
              "回 Idle 后 mouth_pose=flat");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
