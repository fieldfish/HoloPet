#pragma once
/**
 * audio_event.hpp — 音频运行时事件/状态 (纯 C++, 可单测)  V6
 */

namespace holopet {

/** 音频子系统状态 */
enum class AudioStatus {
    Idle,
    Capturing,   // 录音中 (麦克风)
    Playing      // 播放中 (TTS)
};

inline const char* audioStatusName(AudioStatus s) {
    switch (s) {
        case AudioStatus::Idle:      return "Idle";
        case AudioStatus::Capturing: return "Capturing";
        case AudioStatus::Playing:   return "Playing";
    }
    return "???";
}

/** 电平事件 (TTS/输入监视) */
struct AudioLevelEvent {
    double rms  = 0.0;   // 0..1
    double peak = 0.0;   // 0..1
};

/** 音频设备检测结果 */
struct UsbAudioProbe {
    bool    usb_audio_found = false;   // aplay -l / arecord -l 含 USB Audio
    bool    capture_ok      = false;   // 录音可用
    bool    playback_ok     = false;   // 播放可用
    std::string detail;                // 设备名/卡号摘要
};

} // namespace holopet
