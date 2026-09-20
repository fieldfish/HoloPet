#pragma once
/**
 * audio_config.hpp — USB Audio 检测/录放音配置 (纯 C++, 可单测)  V6
 *
 * 录音/播放由 AI Worker (Python) 执行; C++ 侧负责设备配置校验与
 * bench 脚本参数来源。设备名与阈值全部可配置, 无真实密钥类信息。
 */

#include <string>
#include <stdexcept>

namespace holopet {

struct UsbAudioConfig {
    int    sample_rate = 16000;     // 录音采样率 (STT 友好)
    int    chunk_ms    = 20;        // 音频块时长
    int    record_ms   = 6000;      // 单轮最长录音
    double silence_db  = -35.0;     // VAD 静音阈值 (dBFS)
    int    silence_ms  = 800;       // 静音触发提前结束
    double play_volume = 0.7;       // 播放增益 0..1

    std::string record_device = "";  // arecord -D 参数 (空 = 系统默认)
    std::string play_device   = "";  // aplay -D 参数
    std::string device_pattern = "USB Audio";   // 检测关键字 (aplay -l)

    void validate() const {
        if (sample_rate < 8000 || sample_rate > 48000)
            throw std::invalid_argument("UsbAudioConfig: sample_rate out of range");
        if (record_ms < 500 || record_ms > 60000)
            throw std::invalid_argument("UsbAudioConfig: record_ms out of range");
        if (silence_db > 0.0 || silence_db < -90.0)
            throw std::invalid_argument("UsbAudioConfig: silence_db out of range");
        if (play_volume < 0.0 || play_volume > 1.0)
            throw std::invalid_argument("UsbAudioConfig: play_volume out of [0,1]");
    }

    bool valid() const noexcept {
        try { validate(); return true; }
        catch (...) { return false; }
    }
};

} // namespace holopet
