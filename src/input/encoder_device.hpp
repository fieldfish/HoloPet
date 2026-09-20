#pragma once
/**
 * encoder_device.hpp — EC11 硬件编码器设备  V4-R1
 *
 * V4-R1: 新增 external queue 模式 — 允许共享 main thread 的 InputEventQueue。
 *   - 无参构造: 使用内部 owned_queue_ (向后兼容 V2)
 *   - 接受外部 queue&: 直接 push 外部队列 (V4 单队列)
 *
 * 职责:
 *  - 读取 GPIO (A/B/SW)
 *  - 转换 active-low 电平
 *  - 调用 V1 QuadratureDecoder / ButtonDecoder
 *  - 事件 push 到 InputEventQueue
 *
 * 不包含 Gray 算法 / Button 算法 (复用 V1)。
 * 不操作 SDL / UI。
 */

#include "hardware/gpio_chip.hpp"
#include "input/quadrature_decoder.hpp"
#include "input/button_decoder.hpp"
#include "input/input_queue.hpp"
#include "system/logger.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace holopet {

struct EncoderDeviceConfig {
    std::chrono::microseconds poll_interval{1000};
    std::chrono::microseconds long_press{900'000};
    std::chrono::microseconds debounce{10'000};
};

class EncoderDevice {
public:
    /** 默认: 使用内部 owned queue (向后兼容 V2)。
     * R3-R1 (缺陷 A 同型): 嵌套类型默认实参 T{} 被 GCC/Clang 拒绝 —
     * 拆为无参默认构造 + 显式构造, 行为不变。 */
    EncoderDevice()
        : EncoderDevice(EncoderDeviceConfig(), GpioConfig())
    {}
    explicit EncoderDevice(const EncoderDeviceConfig& dev_cfg,
                           const GpioConfig& gpio_cfg = GpioConfig{})
        : dev_cfg_(dev_cfg)
        , gpio_cfg_(gpio_cfg)
        , gpio_(gpio_cfg_)
        , btn_(static_cast<std::uint64_t>(dev_cfg_.long_press.count()),
               static_cast<std::uint64_t>(dev_cfg_.debounce.count()))
        , output_queue_(&owned_queue_)
    {}

    /** 使用外部共享队列 (V4 单队列) */
    EncoderDevice(InputEventQueue& external_queue,
                  const EncoderDeviceConfig& dev_cfg,
                  const GpioConfig& gpio_cfg = GpioConfig())
        : dev_cfg_(dev_cfg)
        , gpio_cfg_(gpio_cfg)
        , gpio_(gpio_cfg_)
        , btn_(static_cast<std::uint64_t>(dev_cfg_.long_press.count()),
               static_cast<std::uint64_t>(dev_cfg_.debounce.count()))
        , output_queue_(&external_queue)
    {}

    ~EncoderDevice() {
        stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    EncoderDevice(const EncoderDevice&) = delete;
    EncoderDevice& operator=(const EncoderDevice&) = delete;

    void start() {
        if (running_) return;
        if (worker_.joinable()) worker_.join();
        running_ = true;
        try {
            worker_ = std::thread(&EncoderDevice::workerLoop, this);
            LOG_INFO("Encoder") << "worker started (poll="
                                << dev_cfg_.poll_interval.count() << "us)";
        } catch (...) {
            running_ = false;
            throw;
        }
    }

    void stop() { running_ = false; }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
            LOG_INFO("Encoder") << "worker stopped";
        }
    }

    /** 返回当前输出队列 (向后兼容 V2 独立使用场景) */
    InputEventQueue& queue() { return *output_queue_; }

private:
    EncoderDeviceConfig dev_cfg_;
    GpioConfig           gpio_cfg_;
    GpioChip             gpio_;
    QuadratureDecoder    quad_;
    ButtonDecoder        btn_;
    InputEventQueue      owned_queue_;
    InputEventQueue*     output_queue_;

    std::atomic<bool> running_{false};
    std::thread worker_;

    void workerLoop() {
        using namespace std::chrono;

        auto next = steady_clock::now();

        while (running_) {
            bool a_raw = gpio_.read(gpio_cfg_.encoder_a);
            bool b_raw = gpio_.read(gpio_cfg_.encoder_b);
            bool sw_raw = gpio_.read(gpio_cfg_.encoder_sw);

            auto now_us = static_cast<std::uint64_t>(
                duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());

            bool sw_pressed = !sw_raw;

            auto quad_ev = quad_.update(a_raw, b_raw, now_us);
            if (quad_ev.has_value()) {
                output_queue_->push(quad_ev.value());
            }

            auto btn_ev = btn_.update(sw_pressed, now_us);
            if (btn_ev.has_value()) {
                output_queue_->push(btn_ev.value());
            }

            next += dev_cfg_.poll_interval;
            auto now = steady_clock::now();
            if (next > now) {
                std::this_thread::sleep_for(next - now);
            } else {
                next = now + dev_cfg_.poll_interval;
            }
        }
    }
};

} // namespace holopet
