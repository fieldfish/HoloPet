#pragma once
/**
 * quadrature_decoder.hpp — EC11 正交解码器 (纯算法)
 *
 * 使用 16 项 Gray 码查表 + 累加器消抖。
 * 一次机械刻度 = 一次旋转事件 (detent factor = 4)。
 *
 * 禁止依赖: GPIO / thread / SDL / 任何硬件
 */

#include "input_event.hpp"
#include <optional>
#include <cstdint>
#include <array>

namespace holopet {

class QuadratureDecoder {
public:
    /** 每 detent 的步进数 (一个完整 Gray 周期 = 4 步) */
    static constexpr int kDetentSteps = 4;

    /**
     * 输入当前 A/B 引脚电平 + 时间戳(微秒)
     *
     * @param a  A 相电平 (true = 高)
     * @param b  B 相电平 (true = 高)
     * @param time_us  时间戳(微秒), 保留用于未来扩展
     * @return Clockwise / CounterClockwise, 或 std::nullopt (无事件)
     */
    std::optional<EncoderEvent> update(bool a, bool b, std::uint64_t time_us) {
        (void)time_us;  // 保留, 未来可用于时间戳防抖

        std::uint8_t curr = encode(a, b);

        // 首次调用: 只记录初始状态, 不产生事件
        if (!initialized_) {
            prev_state_ = curr;
            initialized_ = true;
            return std::nullopt;
        }

        // 状态未变化 → 无事件
        if (curr == prev_state_) {
            return std::nullopt;
        }

        // 查表
        std::uint8_t idx = static_cast<std::uint8_t>((prev_state_ << 2) | curr);
        int8_t dir = kQuadTable[idx];

        prev_state_ = curr;

        if (dir == 0) {
            // 非法跳变 → 累加器清零
            accumulator_ = 0;
            return std::nullopt;
        }

        // 累加
        accumulator_ += dir;

        // 达到一个完整 detent → 输出事件
        if (accumulator_ >= kDetentSteps) {
            accumulator_ -= kDetentSteps;
            return EncoderEvent::Clockwise;
        }
        if (accumulator_ <= -kDetentSteps) {
            accumulator_ += kDetentSteps;
            return EncoderEvent::CounterClockwise;
        }

        return std::nullopt;
    }

    /** 重置解码器状态 (用于测试/重新初始化) */
    void reset() {
        initialized_ = false;
        prev_state_ = 0;
        accumulator_ = 0;
    }

private:
    bool initialized_ = false;
    std::uint8_t prev_state_ = 0;   // 上一次 AB 状态 (2 bit: bit1=A, bit0=B)
    int accumulator_ = 0;            // 步进累加器

    /** 将 A/B 编码为 2-bit 状态: bit1=A, bit0=B */
    static std::uint8_t encode(bool a, bool b) {
        return static_cast<std::uint8_t>((a ? 0b10 : 0) | (b ? 0b01 : 0));
    }

    /**
     * Gray 码正交解码查表
     *
     * 索引 = (prev_state << 2) | curr_state
     *   值: +1 = CW 步进, -1 = CCW 步进, 0 = 非法/无变化
     *
     * 合法序列:
     *   CW:  00→01→11→10→00
     *   CCW: 00→10→11→01→00
     */
    static constexpr std::array<int8_t, 16> kQuadTable = []() constexpr {
        std::array<int8_t, 16> t{};
        // prev=00(0)
        t[(0b00 << 2) | 0b01] =  1;   // 00→01 CW
        t[(0b00 << 2) | 0b10] = -1;   // 00→10 CCW
        // prev=01(1)
        t[(0b01 << 2) | 0b11] =  1;   // 01→11 CW
        t[(0b01 << 2) | 0b00] = -1;   // 01→00 CCW
        // prev=11(3)
        t[(0b11 << 2) | 0b10] =  1;   // 11→10 CW
        t[(0b11 << 2) | 0b01] = -1;   // 11→01 CCW
        // prev=10(2)
        t[(0b10 << 2) | 0b00] =  1;   // 10→00 CW
        t[(0b10 << 2) | 0b11] = -1;   // 10→11 CCW
        return t;
    }();
};

} // namespace holopet
