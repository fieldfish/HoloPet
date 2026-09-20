#pragma once
/**
 * expression_animation.hpp — R8_R4 表情微动画控制器 (纯 C++, 无 SDL, 确定性)。
 *
 * 与 RuntimeState/Emotion 分离: 只产生渲染参数与相位, 不为一次眨眼新增
 * 业务状态。时间一律取注入的 now_ms (单调毫秒), 随机抽样用注入 seed 的
 * 确定性 PRNG — 禁止系统墙钟/SDL_Delay/全局随机。
 *
 * 每帧输出: eye_scale_y / eye_offset_x/y (以眼睛半径为单位) / mouth_pose
 *           / phase; 相位切换才产生低频审计事件 (§3.6), 不逐帧刷。
 */
#include <cmath>
#include <cstdint>
#include <string>

#include "agent/agent_event.hpp"

namespace holopet {

enum class FaceAnimPhase {
    Idle,
    BlinkClosing,
    BlinkClosed,
    BlinkOpening,
    ThinkingHold,
    Returning,
};

enum class MouthPose {
    Flat,
    Pout,
    Speech,
};

enum class ThinkingDir {
    UpperLeft,
    UpperRight,
    UpperCenter,
};

/** 每帧渲染参数 (有界)。 */
struct FaceAnimFrame {
    double eye_scale_y = 1.0;      // 0.05..1.0
    double eye_offset_x = 0.0;     // 以眼睛半径为单位
    double eye_offset_y = 0.0;     // 以眼睛半径为单位 (负=向上)
    MouthPose mouth_pose = MouthPose::Flat;
    FaceAnimPhase phase = FaceAnimPhase::Idle;
    bool blink_visible = false;    // 渲染层据此压缩/闭合眼睛
};

/** 低频审计事件 (§3.6)。 */
struct FaceAnimAudit {
    bool emitted = false;
    std::string event;             // blink_start|blink_end|thinking_pose|baseline_restored
    std::string direction;         // thinking_pose: upper_left/upper_right/upper_center
    RuntimeState state = RuntimeState::Idle;
    int64_t t_ms = 0;
};

/** 动画时序参数 (顶层结构体: GCC 14 要求默认参数处类完整, 不得嵌在类内用 NSDMI)。 */
struct FaceAnimTiming {
        int64_t blink_min_ms = 4500;
        int64_t blink_max_ms = 8000;
        int64_t blink_close_ms = 70;
        int64_t blink_hold_ms = 40;
        int64_t blink_open_ms = 90;
        int64_t double_blink_min_ms = 110;
        int64_t double_blink_max_ms = 180;
        int64_t thinking_delay_min_ms = 250;
        int64_t thinking_delay_max_ms = 450;
        int64_t pose_hold_min_ms = 700;
        int64_t pose_hold_max_ms = 1200;
        int64_t return_min_ms = 120;
        int64_t return_max_ms = 180;
        double double_blink_prob = 0.12;
        double think_x_min = 0.25;   // |X| 下限 (半径倍)
        double think_x_max = 0.45;
        double think_y_min = 0.20;   // 向上偏移 (半径倍)
        double think_y_max = 0.35;
};

class ExpressionAnimationController {
public:
    explicit ExpressionAnimationController(uint64_t seed = 12345u,
                                           FaceAnimTiming t = FaceAnimTiming())
        : t_(t), rng_state_(seed ? seed : 1u) {}

    // ---- 状态输入 ----
    void setState(RuntimeState state, int64_t now_ms) {
        if (state == state_) return;
        state_ = state;
        if (state == RuntimeState::Thinking) {
            enterThinking(now_ms);
        } else if (state == RuntimeState::Speaking) {
            phase_ = FaceAnimPhase::Idle;
            frame_.mouth_pose = MouthPose::Speech;
            cur_x_ = cur_y_ = 0.0;
            frame_.eye_scale_y = 1.0;
            frame_.eye_offset_x = frame_.eye_offset_y = 0.0;
            frame_.blink_visible = false;
            frame_.phase = FaceAnimPhase::Idle;
        } else if (state == RuntimeState::Listening) {
            phase_ = FaceAnimPhase::Idle;
            frame_.mouth_pose = MouthPose::Flat;
            // Listening 进入允许快眨一次
            phase_ = FaceAnimPhase::BlinkClosing;
            phase_start_ms_ = now_ms;
            frame_.blink_visible = true;
        } else {
            // Idle/Error/Offline: 若此前在思考/动画中 → 有界回归基准
            if (cur_x_ != 0.0 || cur_y_ != 0.0 || frame_.eye_scale_y < 1.0
                || frame_.mouth_pose == MouthPose::Pout
                || frame_.mouth_pose == MouthPose::Speech) {
                beginReturn(now_ms);
            } else {
                phase_ = FaceAnimPhase::Idle;
                frame_.mouth_pose = MouthPose::Flat;
            }
        }
    }

    /** R8_R4 §3.4: 首个 LLM 内容到达/取消/错误/超时 — 结束思考视觉,
     *  120~180ms 内回基准 (业务状态可仍为 Thinking)。 */
    void notifyContentStarted(int64_t now_ms) {
        if (state_ != RuntimeState::Thinking) return;
        if (cur_x_ != 0.0 || cur_y_ != 0.0 || frame_.mouth_pose != MouthPose::Flat) {
            beginReturn(now_ms);
        }
        thinking_done_ = true;
    }

    /** 每帧调用。 */
    FaceAnimAudit tick(int64_t now_ms, bool paused = false) {
        // R8_R4_R3_R4_R5: 菜单/时钟页面暂停眨眼 (保持基准帧, 不进入眨眼相位)
        if (paused) {
            frame_.eye_scale_y = 1.0;
            frame_.eye_offset_x = 0.0;
            frame_.eye_offset_y = 0.0;
            frame_.mouth_pose = MouthPose::Flat;
            frame_.blink_visible = false;
            return FaceAnimAudit{};
        }
        FaceAnimAudit a;
        switch (phase_) {
        case FaceAnimPhase::Idle:
            if (state_ == RuntimeState::Idle && now_ms >= next_blink_ms_) {
                beginBlink(now_ms, false);
                a = emit("blink_start", "", now_ms);
            } else if (state_ == RuntimeState::Thinking) {
                thinkingTick(now_ms, a);
            }
            break;
        case FaceAnimPhase::BlinkClosing:
            progressBlink(now_ms);
            if (now_ms - phase_start_ms_ >= t_.blink_close_ms) {
                phase_ = FaceAnimPhase::BlinkClosed;
                phase_start_ms_ = now_ms;
                frame_.eye_scale_y = 0.05;      // 最薄不消失
                frame_.blink_visible = true;
            }
            break;
        case FaceAnimPhase::BlinkClosed:
            if (now_ms - phase_start_ms_ >= t_.blink_hold_ms) {
                phase_ = FaceAnimPhase::BlinkOpening;
                phase_start_ms_ = now_ms;
            }
            break;
        case FaceAnimPhase::BlinkOpening:
            progressBlinkOpen(now_ms);
            if (now_ms - phase_start_ms_ >= t_.blink_open_ms) {
                phase_ = FaceAnimPhase::Idle;
                frame_.eye_scale_y = 1.0;
                frame_.blink_visible = false;
                a = emit("blink_end", "", now_ms);
                if (pending_double_) {
                    pending_double_ = false;
                    int64_t gap = sample(t_.double_blink_min_ms,
                                         t_.double_blink_max_ms);
                    next_blink_ms_ = now_ms + gap;
                } else {
                    scheduleNextBlink(now_ms);
                }
            }
            break;
        case FaceAnimPhase::ThinkingHold:
            thinkingTick(now_ms, a);
            break;
        case FaceAnimPhase::Returning:
            progressReturn(now_ms);
            if (now_ms - return_start_ms_ >= t_.return_max_ms
                || (cur_x_ == 0.0 && cur_y_ == 0.0
                    && frame_.eye_scale_y >= 1.0)) {
                phase_ = FaceAnimPhase::Idle;
                frame_.mouth_pose = MouthPose::Flat;
                cur_x_ = cur_y_ = 0.0;
                frame_.eye_scale_y = 1.0;
                frame_.eye_offset_x = frame_.eye_offset_y = 0.0;
                a = emit("baseline_restored", "", now_ms);
                if (state_ == RuntimeState::Idle) scheduleNextBlink(now_ms);
            }
            break;
        }
        frame_.phase = phase_;
        return a;
    }

    FaceAnimFrame frame() const { return frame_; }
    ThinkingDir thinkingDir() const { return dir_; }
    int poseChangesThisEpisode() const { return pose_changes_; }

    /** 确定性 PRNG (xorshift64*; 测试可复现)。 */
    double nextRand() {
        uint64_t x = rng_state_;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        rng_state_ = x;
        return static_cast<double>(x * 2685821657736338717ULL >> 11)
               / 9007199254740992.0;   // [0,1)
    }

private:
    int64_t sample(int64_t lo, int64_t hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int64_t>(nextRand() * (hi - lo + 1)) % (hi - lo + 1);
    }

    void scheduleNextBlink(int64_t now_ms) {
        next_blink_ms_ = now_ms + sample(t_.blink_min_ms, t_.blink_max_ms);
        pending_double_ = nextRand() < t_.double_blink_prob;
    }

    void beginBlink(int64_t now_ms, bool) {
        phase_ = FaceAnimPhase::BlinkClosing;
        phase_start_ms_ = now_ms;
        frame_.blink_visible = true;
    }

    void progressBlink(int64_t now_ms) {
        int64_t e = now_ms - phase_start_ms_;
        double f = t_.blink_close_ms > 0
            ? static_cast<double>(e) / t_.blink_close_ms : 1.0;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        frame_.eye_scale_y = 1.0 - f * 0.95;   // → 0.05
        frame_.blink_visible = true;
    }

    void progressBlinkOpen(int64_t now_ms) {
        int64_t e = now_ms - phase_start_ms_;
        double f = t_.blink_open_ms > 0
            ? static_cast<double>(e) / t_.blink_open_ms : 1.0;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        frame_.eye_scale_y = 0.05 + f * 0.95;  // 0.05 → 1.0
        frame_.blink_visible = true;
    }

    void enterThinking(int64_t now_ms) {
        phase_ = FaceAnimPhase::Idle;
        phase_start_ms_ = now_ms;
        thinking_dir_picked_ = false;
        thinking_done_ = false;
        pose_changes_ = 0;
        cur_x_ = cur_y_ = 0.0;
        frame_.eye_scale_y = 1.0;
        frame_.mouth_pose = MouthPose::Pout;   // 轻微噘嘴随思考持续
        frame_.blink_visible = false;
    }

    void pickThinkingDir(int64_t now_ms) {
        int d = sample(0, 2);
        dir_ = d == 0 ? ThinkingDir::UpperLeft
             : d == 1 ? ThinkingDir::UpperRight : ThinkingDir::UpperCenter;
        double mag_x = t_.think_x_min
            + nextRand() * (t_.think_x_max - t_.think_x_min);
        double mag_y = t_.think_y_min
            + nextRand() * (t_.think_y_max - t_.think_y_min);
        double sx = (dir_ == ThinkingDir::UpperLeft) ? -1.0
                  : (dir_ == ThinkingDir::UpperRight) ? 1.0 : 0.0;
        think_target_x_ = sx * mag_x;
        think_target_y_ = -mag_y;              // 负 = 向上
        cur_x_ = think_target_x_;
        cur_y_ = think_target_y_;
        frame_.eye_offset_x = cur_x_;
        frame_.eye_offset_y = cur_y_;
        thinking_dir_picked_ = true;
        phase_ = FaceAnimPhase::ThinkingHold;
        phase_start_ms_ = now_ms;
        (void)now_ms;
    }

    void thinkingTick(int64_t now_ms, FaceAnimAudit& a) {
        if (thinking_done_) return;   // 内容已到 → 视觉已回基准
        if (!thinking_dir_picked_) {
            if (now_ms - phase_start_ms_ >= t_.thinking_delay_min_ms
                && now_ms - phase_start_ms_ >=
                   static_cast<int64_t>(t_.thinking_delay_min_ms
                       + nextRand()
                       * (t_.thinking_delay_max_ms - t_.thinking_delay_min_ms))) {
                pickThinkingDir(now_ms);
                a = emit("thinking_pose",
                         dir_ == ThinkingDir::UpperLeft ? "upper_left"
                         : dir_ == ThinkingDir::UpperRight ? "upper_right"
                         : "upper_center", now_ms);
            }
            return;
        }
        // 姿态保持: 到点换向 (每轮 ≤2 次)
        int64_t hold = sample(t_.pose_hold_min_ms, t_.pose_hold_max_ms);
        if (now_ms - phase_start_ms_ >= hold && pose_changes_ < 2) {
            ++pose_changes_;
            int d = sample(0, 2);
            dir_ = d == 0 ? ThinkingDir::UpperLeft
                 : d == 1 ? ThinkingDir::UpperRight : ThinkingDir::UpperCenter;
            double mag_x = t_.think_x_min
                + nextRand() * (t_.think_x_max - t_.think_x_min);
            double mag_y = t_.think_y_min
                + nextRand() * (t_.think_y_max - t_.think_y_min);
            double sx = (dir_ == ThinkingDir::UpperLeft) ? -1.0
                      : (dir_ == ThinkingDir::UpperRight) ? 1.0 : 0.0;
            think_target_x_ = sx * mag_x;
            think_target_y_ = -mag_y;
            phase_start_ms_ = now_ms;
            a = emit("thinking_pose",
                     dir_ == ThinkingDir::UpperLeft ? "upper_left"
                     : dir_ == ThinkingDir::UpperRight ? "upper_right"
                     : "upper_center", now_ms);
        }
        // 姿态直接落位；状态切换时使用有界的基线恢复。
        cur_x_ = think_target_x_;
        cur_y_ = think_target_y_;
        frame_.eye_offset_x = cur_x_;
        frame_.eye_offset_y = cur_y_;
        frame_.mouth_pose = MouthPose::Pout;
    }

    void beginReturn(int64_t now_ms) {
        phase_ = FaceAnimPhase::Returning;
        return_start_ms_ = now_ms;
        return_from_x_ = cur_x_;
        return_from_y_ = cur_y_;
        return_from_scale_ = frame_.eye_scale_y;
    }

    void progressReturn(int64_t now_ms) {
        int64_t e = now_ms - return_start_ms_;
        int64_t dur = sample(t_.return_min_ms, t_.return_max_ms);
        double f = dur > 0 ? static_cast<double>(e) / dur : 1.0;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        cur_x_ = return_from_x_ * (1.0 - f);
        cur_y_ = return_from_y_ * (1.0 - f);
        frame_.eye_offset_x = cur_x_;
        frame_.eye_offset_y = cur_y_;
        frame_.eye_scale_y = return_from_scale_ + (1.0 - return_from_scale_) * f;
        frame_.blink_visible = false;
    }

    FaceAnimAudit emit(const std::string& ev, const std::string& dir,
                       int64_t now_ms) {
        FaceAnimAudit a;
        a.emitted = true;
        a.event = ev;
        a.direction = dir;
        a.state = state_;
        a.t_ms = now_ms;
        return a;
    }

    FaceAnimTiming t_;
    uint64_t rng_state_;
    RuntimeState state_ = RuntimeState::Idle;
    FaceAnimPhase phase_ = FaceAnimPhase::Idle;
    FaceAnimFrame frame_;
    ThinkingDir dir_ = ThinkingDir::UpperCenter;
    int64_t phase_start_ms_ = 0;
    int64_t next_blink_ms_ = 0;
    int pose_changes_ = 0;
    bool thinking_dir_picked_ = false;
    bool thinking_done_ = false;
    bool pending_double_ = false;
    int64_t return_start_ms_ = 0;
    double return_from_x_ = 0.0;
    double return_from_y_ = 0.0;
    double return_from_scale_ = 1.0;
    double think_target_x_ = 0.0;
    double think_target_y_ = 0.0;
    double cur_x_ = 0.0;
    double cur_y_ = 0.0;
};

} // namespace holopet
