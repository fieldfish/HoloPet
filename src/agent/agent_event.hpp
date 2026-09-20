#pragma once
/**
 * agent_event.hpp — AI 对话事件 (V6)
 *
 * C++ 主程序与 AI Worker 之间传递的运行时事件。
 * 纯 C++ 逻辑, 禁止依赖 SDL / 网络 / thread。
 */

#include <cstdint>
#include <string>

namespace holopet {

/** 运行状态 (V6 生产状态机, 与 AppState/V4 演示状态分离) */
enum class RuntimeState {
    Idle,        // 待机
    Listening,   // 录音中
    Thinking,    // STT/LLM 处理中
    Speaking,    // TTS 播放中
    Error,       // 本轮失败 (可恢复)
    Offline      // Worker 不可用 (未连接/崩溃)
};

inline const char* runtimeStateName(RuntimeState s) {
    switch (s) {
        case RuntimeState::Idle:      return "Idle";
        case RuntimeState::Listening: return "Listening";
        case RuntimeState::Thinking:  return "Thinking";
        case RuntimeState::Speaking:  return "Speaking";
        case RuntimeState::Error:     return "Error";
        case RuntimeState::Offline:   return "Offline";
    }
    return "???";
}

/** 情绪 (与运行状态正交; Speaking+Happy 可并存)。
 * R8_R4_R3_R4_R5: 九种正式参考表情 — 在既有七种上追加五种
 * (馋 Craving / 无语 Speechless / 喜悦 Joy / 生气 Angry / 可爱 Cute),
 * 枚举值只追加不重排 (线级语义向后兼容)。 */
enum class Emotion {
    Neutral,
    Happy,        // 开心
    Curious,      // 疑问
    Surprised,    // 惊讶
    Sad,
    Sleepy,
    Concerned,    // 为难
    Craving,      // 馋
    Speechless,   // 无语
    Joy,          // 喜悦
    Angry,        // 生气
    Cute          // 可爱
};

inline const char* emotionName(Emotion e) {
    switch (e) {
        case Emotion::Neutral:    return "Neutral";
        case Emotion::Happy:      return "Happy";
        case Emotion::Curious:    return "Curious";
        case Emotion::Surprised:  return "Surprised";
        case Emotion::Sad:        return "Sad";
        case Emotion::Sleepy:     return "Sleepy";
        case Emotion::Concerned:  return "Concerned";
        case Emotion::Craving:    return "Craving";
        case Emotion::Speechless: return "Speechless";
        case Emotion::Joy:        return "Joy";
        case Emotion::Angry:      return "Angry";
        case Emotion::Cute:       return "Cute";
    }
    return "???";
}

/** 字符串 → 情绪; 未知稳定回退 Neutral (R8_R4_R3_R4_R5 §5)。 */
inline Emotion emotionFromName(const std::string& v) {
    if (v == "happy")       return Emotion::Happy;
    if (v == "curious")     return Emotion::Curious;
    if (v == "surprised")   return Emotion::Surprised;
    if (v == "sad")         return Emotion::Sad;
    if (v == "sleepy")      return Emotion::Sleepy;
    if (v == "concerned")   return Emotion::Concerned;
    if (v == "craving")     return Emotion::Craving;
    if (v == "speechless")  return Emotion::Speechless;
    if (v == "joy")         return Emotion::Joy;
    if (v == "angry")       return Emotion::Angry;
    if (v == "cute")        return Emotion::Cute;
    return Emotion::Neutral;
}

/** 来自 AI Worker 的事件 */
enum class AgentEventType {
    WorkerOnline,     // IPC 连接建立
    WorkerLost,       // IPC 断开/崩溃
    TurnStarted,      // 本轮开始
    StateChanged,     // worker 报告运行状态 (listening/thinking/speaking/...)
    Transcript,       // 用户听写文本
    ContentChunk,     // R4-R1: 回答正文分片 (逐块追加, 不提交)
    ResponseComplete, // R4-R1: 完整回答结束 (此时才提交聚合内容)
    TtsStarted,       // R8_R2_R3 (P0-3): TTS 开始 (text-only 合同必须能观察并判零)
    TtsLevel,         // TTS 音量电平
    TtsFinished,      // R8_R2_R3 (P0-3): TTS 结束
    TurnDone,         // 本轮结束
    TurnError,        // 本轮失败
    TurnCancelled,    // 本轮被取消
    Expression,       // 情绪更新 (工具调用 set_expression 产生)
    ToolRequest,      // R8_R4: agentd 本地功能工具请求 (text=完整请求 JSON)
    ToolCall,         // R5_R4: 工具执行反馈 (text=工具名, 展示简短动作)
    Waiting           // R8_R4_R2: 慢回答等待提示 (text=提示文本)
};

struct AgentEvent {
    AgentEventType type = AgentEventType::WorkerOnline;
    RuntimeState   state = RuntimeState::Idle;   // StateChanged
    Emotion        emotion = Emotion::Neutral;   // Expression
    std::string    text;                          // Transcript / ContentChunk / TurnError
    double         level = 0.0;                   // TtsLevel rms 0..1
    /**
     * R8_R2_R3 (P0-2): 该事件所属原始 WorkerMessage 的 request_id。
     * 由 toAgentEvent 逐条赋值 (通过 RequestIdFilter 之后), 审计据此逐事件
     * 证明轮次归属, 不再依赖主线程共享变量。非轮次事件 (WorkerOnline 等) 为空。
     */
    std::string    request_id;
    /**
     * R8_R2_R6 (A): 错误事件的公开稳定错误码 — 优先取协议 code (llm_*)。
     * text 只作内部说明保留, 不得作为上层错误身份。
     */
    std::string    error_code;
};

} // namespace holopet
