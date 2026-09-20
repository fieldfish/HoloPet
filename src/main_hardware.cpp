/**
 * main_hardware.cpp — HoloPet V2 硬件编码器测试程序  V2-R1
 *
 * R1 修复: signal handler 只设置退出标志, 不做 Logger 调用
 *
 * 仅当 BUILD_HARDWARE=ON 时编译。
 * Pi 实机运行: 读取 EC11 旋钮 → 输出事件到 console。
 */

#include "input/encoder_device.hpp"
#include "system/logger.hpp"

#include <atomic>
#include <csignal>
#include <iostream>

using namespace holopet;

static volatile std::sig_atomic_t g_running = 1;

void signalHandler(int) {
    g_running = 0;
}

const char* eventName(EncoderEvent ev) {
    switch (ev) {
        case EncoderEvent::Clockwise:        return "CW";
        case EncoderEvent::CounterClockwise: return "CCW";
        case EncoderEvent::Pressed:          return "Pressed";
        case EncoderEvent::LongPressed:      return "LongPressed";
    }
    return "?";
}

int main() try {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    LOG_INFO("Main") << "HoloPet V2 Encoder Test (Pi5 profile: "
                     << "pinctrl-rp1 自动发现, A=27/B=17/SW=22)";

    // 生产 Pi 5 profile (V6-R2: 三入口统一, 不再依赖 auto 探测)
    GpioConfig gpio_cfg = GpioConfig::forPi5();
    EncoderDeviceConfig dev_cfg;
    dev_cfg.poll_interval = std::chrono::microseconds(1000);

    EncoderDevice encoder(dev_cfg, gpio_cfg);

    // 启动 worker
    encoder.start();
    LOG_INFO("Main") << "Ready. Rotate / press EC11... (Ctrl-C to exit)";

    // 主循环: 消费事件
    EncoderEvent ev;
    while (g_running) {
        while (encoder.queue().tryPop(ev)) {
            LOG_INFO("Event") << eventName(ev);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 正常退出流程
    LOG_INFO("Main") << "shutting down...";
    encoder.stop();
    encoder.join();

    LOG_INFO("Main") << "goodbye.";
    return 0;

} catch (const std::exception& ex) {
    LOG_ERROR("Main") << "fatal: " << ex.what();
    return 1;
}
