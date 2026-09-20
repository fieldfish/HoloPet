#pragma once
/**
 * input_queue.hpp — 线程安全输入事件队列 (V1.0.6 实现)
 */

#include "input_event.hpp"
#include <queue>
#include <mutex>

namespace holopet {

class InputEventQueue {
public:
    /** 压入事件 */
    void push(EncoderEvent event) {
        // V1.0.6 实现
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(event);
    }

    /** 尝试取出事件, 成功返回 true */
    bool tryPop(EncoderEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        event = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<EncoderEvent> queue_;
    std::mutex mutex_;
};

} // namespace holopet
