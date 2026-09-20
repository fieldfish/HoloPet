#pragma once
/**
 * ai_worker_client.hpp — AI Worker IPC 客户端 (V6)
 *
 * 职责: 非阻塞连接 127.0.0.1 (JSON Lines)、后台读线程、事件队列、
 *      部分读取缓冲、过期 request_id 丢弃、崩溃后重连。
 * 保证: 任何网络操作不进入 SDL 主线程。
 *
 * 依赖: C++20 标准库 (thread/atomic/socket 由实现层提供, 此头只声明接口)
 */

#include "agent/agent_event.hpp"
#include "ipc/ai_worker_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace holopet {

/** Worker 消息 → AgentEvent 映射 (供 C++ 状态机消费) */
inline std::optional<AgentEvent> toAgentEvent(const WorkerMessage& m) {
    AgentEvent ev;
    if (!m.known_type) return std::nullopt;   // 未知 type 忽略
    // R8_R2_R3 (P0-2): 每条轮次事件携带其原始 request_id (过滤已在上游完成)
    ev.request_id = m.request_id;
    if (m.type == "state") {
        ev.type = AgentEventType::StateChanged;
        if (m.value == "listening")       ev.state = RuntimeState::Listening;
        else if (m.value == "thinking")   ev.state = RuntimeState::Thinking;
        else if (m.value == "speaking")   ev.state = RuntimeState::Speaking;
        else if (m.value == "error")      ev.state = RuntimeState::Error;
        else                              ev.state = RuntimeState::Idle;
        return ev;
    }
    if (m.type == "transcript") { ev.type = AgentEventType::Transcript; ev.text = m.text; return ev; }
    // R4-R1 (D1): content/response_complete 进入真实事件链 (不再只是 known type)
    if (m.type == "content") { ev.type = AgentEventType::ContentChunk; ev.text = m.text; return ev; }
    if (m.type == "response_complete") { ev.type = AgentEventType::ResponseComplete; return ev; }
    if (m.type == "tts_started")  { ev.type = AgentEventType::TtsStarted;  return ev; }
    if (m.type == "tts_level")  { ev.type = AgentEventType::TtsLevel;  ev.level = m.rms;  return ev; }
    if (m.type == "tts_finished") { ev.type = AgentEventType::TtsFinished; return ev; }
    if (m.type == "done")       { ev.type = AgentEventType::TurnDone;    return ev; }
    if (m.type == "error") {
        ev.type = AgentEventType::TurnError;
        ev.text = m.text;   // 说明/内部详情保留
        // R8_R2_R6 (A): 上层错误身份优先取协议稳定 code;
        // 历史消息无 code 时安全回退 text (受契约测试覆盖)。
        ev.error_code = m.code.empty() ? m.text : m.code;
        return ev;
    }
    if (m.type == "cancel")     { ev.type = AgentEventType::TurnCancelled; return ev; }
    // R8_R4: 工具请求 — text 携带完整 tool_request JSON (主线程交 ToolBridge)
    if (m.type == "waiting")    { ev.type = AgentEventType::Waiting; ev.text = m.text; return ev; }
    if (m.type == "tool_request") {
        ev.type = AgentEventType::ToolRequest;
        ev.text = m.text;
        return ev;
    }
    // R5_R4 (G): 工具执行反馈 — value=工具名, 主线程映射简短动作标签
    if (m.type == "tool_call") {
        ev.type = AgentEventType::ToolCall;
        ev.text = m.value;
        return ev;
    }
    if (m.type == "expression") {
        ev.type = AgentEventType::Expression;
        // R8_R4_R3_R4_R5: 未知情绪稳定回退 Neutral (九表情+向后兼容)
        ev.emotion = emotionFromName(m.value);
        return ev;
    }
    return std::nullopt;
}

/**
 * 行缓冲: 处理部分读取 (流式 JSON Lines 拆行)。
 * 线程安全 (读线程 append, 主线程 extract)。
 * R4: 无换行的尾部超过 kMaxLineBytes → 清空缓冲并返回 overflow=true
 * (与 Python protocol.read_lines 的 line_too_long 语义一致)。
 */
class LineBuffer {
public:
    static constexpr size_t kMaxLineBytes = 1 * 1024 * 1024;

    /** 追加一段字节流 (可能包含 0..n 个完整行)。
     * R4-R1 (D4): 超长半包只丢尾部 — 已缓冲的合法完整行必须保留。
     * @return true=部分包超限 (尾部已丢弃), false=正常 */
    bool append(const char* data, size_t len) {
        std::lock_guard<std::mutex> lk(m_);
        buf_.append(data, len);
        const size_t nl = buf_.rfind('\n');
        const size_t tail = (nl == std::string::npos)
            ? buf_.size() : buf_.size() - nl - 1;
        if (tail > kMaxLineBytes) {
            if (nl == std::string::npos) {
                buf_.clear();                 // 无完整行: 整包丢弃
            } else {
                buf_.resize(nl + 1);          // 保留前面的完整行, 只丢超长尾
            }
            return true;
        }
        return false;
    }

    /** 取出一行 (不含 '\n'); 无完整行返回 nullopt */
    std::optional<std::string> popLine() {
        std::lock_guard<std::mutex> lk(m_);
        size_t pos = buf_.find('\n');
        if (pos == std::string::npos) return std::nullopt;
        std::string line = buf_.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buf_.erase(0, pos + 1);
        return line;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        buf_.clear();
    }

private:
    std::mutex m_;
    std::string buf_;
};

/** 线程安全事件队列 (worker 事件 → 主线程) */
class WorkerEventQueue {
public:
    void push(AgentEvent ev) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() < kMaxQueue) q_.push_back(std::move(ev));
    }
    bool tryPop(AgentEvent& ev) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        ev = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
    }

private:
    static constexpr size_t kMaxQueue = 512;
    std::mutex m_;
    std::deque<AgentEvent> q_;
};

/**
 * 当前轮次的 request_id 过滤 (R2: expression 与其他事件同等过滤,
 * 旧轮 expression 不得污染新轮表情):
 * 只有匹配 active request_id 的消息 (transcript/state/tts/done/error/cancel/expression)
 * 才被投递; 过期 id 直接丢弃 (hello 为握手消息, 不参与过滤)。
 */
class RequestIdFilter {
public:
    void setActive(const std::string& id) {
        std::lock_guard<std::mutex> lk(m_);
        active_ = id;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        active_.clear();
    }
    bool accept(const WorkerMessage& m) const {
        if (m.type == "hello") return true;   // 协议握手
        std::lock_guard<std::mutex> lk(m_);
        if (active_.empty()) return false;    // 无活动轮次 → 丢弃
        return m.request_id == active_;
    }

private:
    mutable std::mutex m_;
    std::string active_;
};

} // namespace holopet
