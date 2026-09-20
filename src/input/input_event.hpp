#pragma once
/**
 * input_event.hpp — 编码器事件枚举 (唯一定义处)
 *
 * 全项目只此一处定义 EncoderEvent。
 * 禁止在其他文件中重复声明。
 */

namespace holopet {

enum class EncoderEvent {
    Clockwise,        // 顺时针旋转
    CounterClockwise, // 逆时针旋转
    Pressed,          // 短按
    LongPressed       // 长按
};

} // namespace holopet
