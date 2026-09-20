/**
 * test_audio_config.cpp — V6 USB 音频配置校验测试
 */

#include "audio/audio_config.hpp"
#include "audio/audio_event.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_audio_config (V6) ===\n\n";

    UsbAudioConfig cfg;
    check(cfg.valid(), "默认配置有效");
    check(cfg.sample_rate == 16000, "默认 16kHz");
    check(cfg.record_ms == 6000, "默认录音 6s 上限");
    check(cfg.device_pattern == "USB Audio", "检测关键字 USB Audio");

    {
        UsbAudioConfig bad = cfg; bad.sample_rate = 1000;
        check(!bad.valid(), "sample_rate 1000 无效");
        bad = cfg; bad.record_ms = 100;
        check(!bad.valid(), "record_ms 100 无效");
        bad = cfg; bad.silence_db = 10.0;
        check(!bad.valid(), "silence_db +10 无效");
        bad = cfg; bad.play_volume = 1.5;
        check(!bad.valid(), "play_volume 1.5 无效");
    }

    // 异常路径
    {
        UsbAudioConfig bad; bad.sample_rate = 0;
        bool threw = false;
        try { bad.validate(); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "validate() 抛 invalid_argument");
    }

    // 事件结构
    AudioLevelEvent lv; lv.rms = 0.5; lv.peak = 0.9;
    check(lv.rms == 0.5 && lv.peak == 0.9, "AudioLevelEvent 字段");
    check(std::string(audioStatusName(AudioStatus::Idle)) == "Idle", "状态名 Idle");
    check(std::string(audioStatusName(AudioStatus::Capturing)) == "Capturing", "状态名 Capturing");
    check(std::string(audioStatusName(AudioStatus::Playing)) == "Playing", "状态名 Playing");

    UsbAudioProbe probe;
    check(!probe.usb_audio_found, "探测默认未发现 (实机判定)");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
