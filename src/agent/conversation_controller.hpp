#pragma once
/**
 * conversation_controller.hpp — V6 生产对话控制器 (纯 C++, 可单测)
 *
 * 职责:
 *  - 生产状态机 (RuntimeState) 与 V4 AppController 演示状态完全分离
 *  - EC11 生产行为: 短按(Idle 开始/Listening 结束录音/Speaking 停播)
 *                  长按取消、旋转调音量 + 音量层计时
 *  - AI Worker 事件 → 状态/情绪更新; 轮次超时; 单调时钟 (时间由外部传入)
 *  - 情绪与状态分离: emotion 独立字段 (Speaking + Happy 可并存)
 *
 * R4-R1 (D1/D2/D10):
 *  - 文本三分: user_transcript_ / pending_answer_ (content 逐块累积) /
 *    committed_answer_ (response_complete 才提交); 新轮/cancel/error/disconnect
 *    有明确清理规则。
 *  - Thinking 超时以"本次进入 Thinking 的时刻"为基准 (markThinking 或
 *    StateChanged Thinking 首次进入时锚定; 重复 Thinking 不重置锚点 —
 *    防止对端重发事件把超时永远推后)。
 *  - Expression 更新独立情绪字段 (set_expression 工具 → expression 事件)。
 *
 * 禁止依赖: SDL / 网络 / thread。仅由主线程访问。
 */

#include "agent/agent_event.hpp"
#include "input/input_event.hpp"

#include <cstdint>

namespace holopet {

class ConversationController {
public:
    struct Config {
        int64_t long_press_ms = 800;    // 长按判定阈值
        // R8_R3_R3_R1 (修复二): 30000→210000 — Thinking 阶段 Error 只能来自
        // worker TurnError/断线/最终超时; deepseek-v4-pro 实测 ~80s。
        int64_t turn_timeout_ms = 210000; // 单轮超时 (Thinking 阶段)
        int64_t volume_layer_ms = 1200;  // 音量层显示时长
        float volume = 0.5f;
        float volume_step = 0.05f;
        float volume_min = 0.0f;
        float volume_max = 1.0f;
    };

    /** R4-R1: 累积回答的字节上限 (UTF-8 原始字节; 超限丢弃后续分片) */
    static constexpr size_t kMaxAnswerBytes = 64 * 1024;

    /** 控制器输出命令 (由主程序消费) */
    enum class Command {
        None,
        StartTurn,      // 开始新一轮对话
        StopRecording,  // 提前结束录音
        StopSpeak,      // 停止当前播放
        CancelTurn      // 取消本轮并回 Idle
    };

    // R3-R1 (缺陷 A): 嵌套 Config 在类定义未完成时作默认实参 (Config{})
    // 被 GCC/Clang 拒绝 (MSVC 接受) — 拆为无参默认构造 + 显式构造,
    // 行为/默认值不变。
    ConversationController() = default;
    explicit ConversationController(const Config& cfg) : cfg_(cfg) {}

    /** 当前运行状态 */
    RuntimeState state() const noexcept { return state_; }
    /** 当前情绪 (独立于状态) */
    Emotion emotion() const noexcept { return emotion_; }
    /** 当前音量 */
    float volume() const noexcept { return cfg_.volume; }
    /** 音量层是否可见 (短暂显示) */
    bool volumeLayerVisible(int64_t now_ms) const noexcept {
        return (now_ms - last_volume_ms_) < cfg_.volume_layer_ms && now_ms >= last_volume_ms_;
    }
    /** 用户听写文本 (R4-R1: 与回答分离) */
    const std::string& userTranscript() const noexcept { return user_transcript_; }
    /** 已提交回答 (response_complete 后才非"无效"); 无 content 的空回答也有效 */
    const std::string& committedAnswer() const noexcept { return committed_answer_; }
    bool answerValid() const noexcept { return answer_valid_; }
    const std::string& waitingHint() const noexcept { return waiting_hint_; }
    /** 正在累积的回答 (response_complete 前) */
    const std::string& pendingAnswer() const noexcept { return pending_answer_; }
    /** UI 显示正文: 优先已提交回答; 未提交前退回用户听写 */
    const std::string& displayText() const noexcept {
        return answer_valid_ ? committed_answer_ : user_transcript_;
    }
    /** 最近 TTS 电平 */
    double ttsLevel() const noexcept { return last_level_; }

    /**
     * 处理 EC11 事件 (生产模式)
     * @param now_ms 单调时钟 (ms), 只用于长按计时, 不阻塞
     * @return 需要执行的命令
     */
    Command handleEncoder(EncoderEvent ev, int64_t now_ms) {
        switch (ev) {
        case EncoderEvent::Clockwise:
            cfg_.volume = clamp(cfg_.volume + cfg_.volume_step,
                                cfg_.volume_min, cfg_.volume_max);
            last_volume_ms_ = now_ms;
            return Command::None;

        case EncoderEvent::CounterClockwise:
            cfg_.volume = clamp(cfg_.volume - cfg_.volume_step,
                                cfg_.volume_min, cfg_.volume_max);
            last_volume_ms_ = now_ms;
            return Command::None;

        case EncoderEvent::Pressed: {
            switch (state_) {
            case RuntimeState::Idle:
            case RuntimeState::Offline:
                // Idle/Offline 短按 → 开始对话 (Offline 无 Worker 时由主程序判定)
                state_ = RuntimeState::Listening;
                return Command::StartTurn;
            case RuntimeState::Listening:
                // R4-R1 (D2): 进入 Thinking 同时锚定超时起点 (本次进入时刻)
                markThinking(now_ms);
                return Command::StopRecording;
            case RuntimeState::Speaking:
                state_ = RuntimeState::Idle;
                return Command::StopSpeak;
            case RuntimeState::Thinking:
            case RuntimeState::Error:
                return Command::None;   // 处理中短按无动作
            }
            return Command::None;
        }

        case EncoderEvent::LongPressed:
            // 长按: 取消本轮并回 Idle
            if (state_ != RuntimeState::Idle && state_ != RuntimeState::Offline) {
                state_ = RuntimeState::Idle;
                return Command::CancelTurn;
            }
            return Command::None;
        }
        return Command::None;
    }

    /**
     * 处理 AI Worker 事件 (R4-R1: now_ms 用于 Thinking 锚点)
     * @return 状态是否变化 (需要重绘)
     */
    bool handleAgent(const AgentEvent& ev, int64_t now_ms = -1) {
        switch (ev.type) {
        case AgentEventType::WorkerOnline:
            if (state_ == RuntimeState::Offline) state_ = RuntimeState::Idle;
            return true;
        case AgentEventType::WorkerLost:
            // 断线: 回 Offline; 未提交回答丢弃 (新轮重新开始)
            state_ = RuntimeState::Offline;
            pending_answer_.clear();
            return true;
        case AgentEventType::TurnStarted:
            // 新轮: 三文本全部复位
            user_transcript_.clear();
            pending_answer_.clear();
            committed_answer_.clear();
            waiting_hint_.clear();
            answer_valid_ = false;
            thinking_anchor_valid_ = false;
            return true;
        case AgentEventType::StateChanged:
            state_ = ev.state;
            // R4-R1 (D2): worker 驱动进入 Thinking 也锚定起点;
            // 已在 Thinking 时重复事件不重置 (固定策略, 有测试)
            if (ev.state == RuntimeState::Thinking && !thinking_anchor_valid_) {
                if (now_ms >= 0) {
                    thinking_since_ms_ = now_ms;
                    thinking_anchor_valid_ = true;
                }
            }
            return true;
        case AgentEventType::Transcript:
            user_transcript_ = ev.text;
            return true;
        case AgentEventType::ContentChunk:
            // R4-R1 (D1): 逐块追加; 超过字节上限丢弃后续分片 (防内存膨胀)
            if (pending_answer_.size() + ev.text.size() <= kMaxAnswerBytes) {
                pending_answer_ += ev.text;
            }
            waiting_hint_.clear();   // 首段内容到达 → 等待提示解除
            return true;
        case AgentEventType::Waiting:
            // R8_R4_R2 (7.3): 慢回答等待提示 (Thinking 期间显示)
            waiting_hint_ = ev.text;
            return true;
        case AgentEventType::ResponseComplete:
            // R4-R1 (D1/D10): 此刻才提交完整回答 (空回答也是有效终态)
            committed_answer_ = std::move(pending_answer_);
            pending_answer_.clear();
            waiting_hint_.clear();
            answer_valid_ = true;
            return true;
        case AgentEventType::TtsLevel:
            last_level_ = ev.level;
            return false;
        case AgentEventType::TtsStarted:
            // R8_R3_R3: 说话动画开始 — 进入/保持 Speaking
            if (state_ != RuntimeState::Offline && state_ != RuntimeState::Error)
                state_ = RuntimeState::Speaking;
            return true;
        case AgentEventType::TtsFinished:
            // R8_R3_R3: 说话动画结束 — 最终 Idle 仍以协议 state/done 为准
            return false;
        case AgentEventType::TurnDone:
            if (state_ != RuntimeState::Offline) state_ = RuntimeState::Idle;
            waiting_hint_.clear();
            return true;
        case AgentEventType::TurnError:
            if (state_ != RuntimeState::Offline) state_ = RuntimeState::Error;
            pending_answer_.clear();   // 错误: 未提交内容丢弃, 已提交保留显示
            waiting_hint_.clear();
            return true;
        case AgentEventType::TurnCancelled:
            // 取消: 回 Idle; 未提交内容丢弃; 已提交回答保留显示
            state_ = RuntimeState::Idle;
            pending_answer_.clear();
            waiting_hint_.clear();
            return true;
        case AgentEventType::Expression:
            emotion_ = ev.emotion;
            return true;
        }
        return false;
    }

    /**
     * 周期 tick (超时/复位)
     * @param now_ms 单调时钟
     */
    bool tick(int64_t now_ms) {
        // R4-R1 (D2): 只有锚点有效才比较 — 程序运行 100s 后进入 Thinking
        // 不会立即超时; 超时以本次进入时刻为基准
        if (state_ == RuntimeState::Thinking && thinking_anchor_valid_ &&
            (now_ms - thinking_since_ms_) >= cfg_.turn_timeout_ms) {
            state_ = RuntimeState::Error;
            pending_answer_.clear();
            return true;
        }
        return false;
    }

    /** 进入 Thinking 时记录起点 (编码器路径; worker 路径见 handleAgent) */
    void markThinking(int64_t now_ms) {
        thinking_since_ms_ = now_ms;
        thinking_anchor_valid_ = true;
        state_ = RuntimeState::Thinking;
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    Config cfg_;
    RuntimeState state_ = RuntimeState::Idle;
    Emotion emotion_ = Emotion::Neutral;
    // R4-R1: 文本三分
    std::string user_transcript_;
    std::string pending_answer_;
    std::string waiting_hint_;   // R8_R4_R2 (7.3): 慢回答等待提示
    std::string committed_answer_;
    bool answer_valid_ = false;
    double last_level_ = 0.0;
    int64_t thinking_since_ms_ = 0;
    bool thinking_anchor_valid_ = false;   // D2: 未锚定时 tick 不得比较
    int64_t last_volume_ms_ = -1 << 30;   // 极早, 初始不可见
};

} // namespace holopet
