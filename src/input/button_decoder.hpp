#pragma once
/**
 * button_decoder.hpp — 按键解码器 (短按/长按/消抖)
 *
 * R1 返工: 候选态 + 稳态算法
 *   先结算已持续足够时间的 candidate，再处理当前新边沿。
 *   长按起点使用 candidate_since + debounce，不依赖轮询频率。
 *
 * 功能:
 *  - 短按: 按下→消抖确认→释放→消抖确认 → Pressed × 1
 *  - 长按: 按下→消抖确认→超阈值 → LongPressed × 1 (不重复)
 *  - 抖动: press bounce / release bounce 均被消抖过滤
 *  - 同一按压绝不产生 Pressed + LongPressed
 */

#include "input_event.hpp"
#include <optional>
#include <cstdint>

namespace holopet {

class ButtonDecoder {
public:
    static constexpr std::uint64_t kDefaultLongPressUs = 900'000;
    static constexpr std::uint64_t kDefaultDebounceUs   = 10'000;

    explicit ButtonDecoder(std::uint64_t long_press_us = kDefaultLongPressUs,
                           std::uint64_t debounce_us = kDefaultDebounceUs)
        : long_press_us_(long_press_us)
        , debounce_us_(debounce_us)
    {}

    /**
     * 输入当前按键电平 + 时间戳(微秒)
     *
     * @param pressed  true=按下, false=释放
     * @param now_us   单调递增时间戳(微秒)
     * @return Pressed / LongPressed, 或 std::nullopt
     */
    std::optional<EncoderEvent> update(bool pressed, std::uint64_t now_us) {
        // ---- 首次调用: 初始化 ----
        if (!initialized_) {
            initialized_ = true;
            stable_     = pressed;
            candidate_  = pressed;
            last_raw_   = pressed;

            if (pressed) {
                // 启动时已经按下: 视为已消抖的按压
                press_start_us_ = now_us;
                long_fired_     = false;
            }
            return std::nullopt;
        }

        std::optional<EncoderEvent> result;

        // ==========================================
        // STEP 1: 先结算已持续足够时间的 candidate
        // ==========================================
        if (candidate_ != stable_) {
            std::uint64_t elapsed = now_us - candidate_since_us_;

            if (debounce_us_ == 0 || elapsed >= debounce_us_) {
                // candidate → stable (在 candidate_since + debounce 时刻正式生效)
                std::uint64_t settle_time = (debounce_us_ == 0)
                    ? candidate_since_us_
                    : candidate_since_us_ + debounce_us_;

                stable_ = candidate_;

                if (stable_) {
                    // 确认按下
                    press_start_us_ = settle_time;
                    long_fired_     = false;
                } else {
                    // 确认释放
                    if (!long_fired_) {
                        result = EncoderEvent::Pressed;
                    }
                    // 长按已触发 → 不补 Pressed
                }
            }
        }

        // ==========================================
        // STEP 2: 处理本次 raw 输入变化
        // ==========================================
        if (pressed != candidate_) {
            candidate_       = pressed;
            candidate_since_us_ = now_us;
        }

        // ==========================================
        // STEP 3: debounce == 0 立即提交
        // ==========================================
        if (debounce_us_ == 0 && candidate_ != stable_) {
            stable_ = candidate_;
            if (stable_) {
                press_start_us_ = now_us;
                long_fired_     = false;
            } else {
                if (!long_fired_ && !result.has_value()) {
                    result = EncoderEvent::Pressed;
                }
            }
        }

        // ==========================================
        // STEP 4: 检测长按
        // ==========================================
        if (stable_ && !long_fired_ && press_start_us_ > 0) {
            if (now_us >= press_start_us_ + long_press_us_) {
                long_fired_ = true;
                // 如果 STEP 1 已经产生了 Pressed，这里覆盖为 LongPressed
                // （正常情况不会同时触发，但防御性处理）
                result = EncoderEvent::LongPressed;
            }
        }

        last_raw_ = pressed;
        return result;
    }

    /** 重置解码器 */
    void reset() {
        initialized_       = false;
        stable_            = false;
        candidate_         = false;
        last_raw_          = false;
        candidate_since_us_ = 0;
        press_start_us_    = 0;
        long_fired_        = false;
    }

private:
    std::uint64_t long_press_us_;
    std::uint64_t debounce_us_;

    bool initialized_        = false;
    bool stable_             = false;   // 已通过消抖的稳态
    bool candidate_          = false;   // 正在尝试变成的新状态
    bool last_raw_           = false;
    std::uint64_t candidate_since_us_ = 0;
    std::uint64_t press_start_us_     = 0;
    bool long_fired_         = false;
};

} // namespace holopet
