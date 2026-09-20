#pragma once
/**
 * ai_command_queue.hpp — 主线程 → IO 线程的命令队列 (V6-R2)
 *
 * SDL 主线程不得直接调用 session; 只向本队列投递 typed 命令。
 * IO owner 线程是唯一消费方。
 */

#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace holopet {

/** 主线程 → IO 线程的 typed 命令 */
enum class AiCommand {
    StartTurn,       // 开始新一轮 (IO 线程生成 request_id 并发送 start_turn)
    StopRecording,   // 提前结束录音 (发送 stop_recording 提示; 实际转写由 worker 完成)
    CancelTurn,      // 取消当前轮
    Shutdown,        // 停止接收命令并准备退出
    SubmitTextTurn,  // R8_R2 (D): 文字输入发起一轮; text 携带 UTF-8 文本
    SendRaw          // R8_R4: text 为完整 JSONL 行 (tool_result/feature_event)
};

/** R8_R2 (D): 带 payload 的命令消息 (唯一可变负载为 SubmitTextTurn.text) */
struct AiCommandMsg {
    AiCommand cmd = AiCommand::Shutdown;
    std::string text;
    std::string meta;   // R8_R4_R3_R4: 附加参数 (如 ai_mode)
};

/** 有界线程安全命令队列 (R4-R1 D8: Shutdown 永不丢) */
class AiCommandQueue {
public:
    /** 入队。@return true=已入队, false=因满被拒 (普通命令);
     *  Shutdown 为高优先级控制命令 — 满时丢弃/合并最旧的普通命令,
     *  绝不丢弃 Shutdown。 */
    bool push(AiCommand cmd) {                 // 兼容旧调用 (无 payload)
        AiCommandMsg m;
        m.cmd = cmd;
        return push(std::move(m));
    }
    bool push(AiCommand cmd, std::string text) {   // R8_R2 (D): 带文本
        AiCommandMsg m;
        m.cmd = cmd;
        m.text = std::move(text);
        return push(std::move(m));
    }
    bool push(AiCommand cmd, std::string text, std::string meta) {
        AiCommandMsg m;
        m.cmd = cmd;
        m.text = std::move(text);
        m.meta = std::move(meta);
        return push(std::move(m));
    }
    bool push(AiCommandMsg msg) {
        std::lock_guard<std::mutex> lk(m_);
        if (msg.cmd == AiCommand::Shutdown) {
            // 已有 Shutdown 则合并 (幂等); 否则挤掉最旧普通命令腾位
            for (const auto& m : q_) {
                if (m.cmd == AiCommand::Shutdown) return true;
            }
            while (q_.size() >= kMaxQueue && !q_.empty()) {
                if (q_.front().cmd == AiCommand::Shutdown) break;  // 上面已查
                q_.pop_front();
            }
            q_.push_back(std::move(msg));
            return true;
        }
        if (q_.size() >= kMaxQueue) return false;   // 普通命令满则拒
        q_.push_back(std::move(msg));
        return true;
    }
    bool tryPop(AiCommand& cmd) {              // 兼容旧调用 (丢弃 text)
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        cmd = q_.front().cmd;
        q_.pop_front();
        return true;
    }
    bool tryPopMsg(AiCommandMsg& msg) {        // R8_R2 (D): 带 payload
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        msg = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return q_.size();
    }

private:
    static constexpr size_t kMaxQueue = 64;
    mutable std::mutex m_;
    std::deque<AiCommandMsg> q_;
};

} // namespace holopet
