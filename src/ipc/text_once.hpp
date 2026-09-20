#pragma once
/**
 * text_once.hpp — R8_R2 (工作包 D) / R8_R2_R1 (工作包 A+B 返工)
 *
 * 1) loadTextOnceFile(): --text-once-file 输入校验 (≤8 KiB, 严格 UTF-8);
 * 2) sha256Hex():        审计用摘要 (纯 C++, 标准 SHA-256);
 * 3) TextTurnLifecycle:  显式状态机 — 以真实终态事件判定文字单轮成功/失败,
 *                        禁止"提交后恰好 Idle"式提前成功;
 * 4) TextEventAudit:     原子 JSONL 事件审计 (写失败 fail closed)。
 *
 * 成功条件全部满足才成功：
 *   已提交(非空 request_id) / 观察到 turn 活动 / ≥1 非空 content /
 *   恰好 1 次 response_complete / 恰好 1 次白名单 expression /
 *   恰好 1 次 done 且 error==0 且 cancel==0 / committed answer 非空 /
 *   全部页面按序展示且末页达到 final hold。
 *
 * 退出码（文档、测试和脚本共用同一表）：
 *   0 成功 | 2 输入无效 | 3 worker 连接超时 | 4 turn/Provider/协议错误 |
 *   5 取消 | 6 等待真实终态超时 | 7 页面展示或审计写入失败
 */

#include "ai_worker_protocol.hpp"   // 复用 isValidUtf8 (RFC 3629 严格版)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace holopet {

/** 读取并校验 --text-once-file; 失败时 err 给出稳定短码。 */
inline std::optional<std::string> loadTextOnceFile(
    const std::string& path, std::string& err, size_t max_bytes = 8192) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "missing_file"; return std::nullopt; }
    std::string raw((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (!f.good() && !f.eof()) { err = "read_failed"; return std::nullopt; }
    if (raw.empty()) { err = "empty_file"; return std::nullopt; }
    if (raw.size() > max_bytes) { err = "too_large"; return std::nullopt; }
    if (!isValidUtf8(raw)) { err = "invalid_utf8"; return std::nullopt; }
    size_t b = raw.find_first_not_of(" \t\r\n");
    size_t e = raw.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) { err = "empty_file"; return std::nullopt; }
    err.clear();
    return raw.substr(b, e - b + 1);
}

// ============================================================
// SHA-256 (审计摘要; 标准实现, 纯 C++)
// ============================================================
namespace detail {

struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    unsigned char buf[64];
    size_t buf_len = 0;

    Sha256() {
        static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                         0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                         0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h, init, sizeof(h));
    }
    static uint32_t rotr(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }
    void process(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,
            0x59f111f1u,0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,
            0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,
            0xc19bf174u,0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
            0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,0x983e5152u,
            0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
            0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,
            0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,
            0xd6990624u,0xf40e3585u,0x106aa070u,0x19a4c116u,0x1e376c08u,
            0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,
            0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i*4]) << 24) | (uint32_t(p[i*4+1]) << 16) |
                   (uint32_t(p[i*4+2]) << 8) | uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const void* data, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        len += n;
        while (n > 0) {
            size_t take = (64 - buf_len < n) ? (64 - buf_len) : n;
            std::memcpy(buf + buf_len, p, take);
            buf_len += take; p += take; n -= take;
            if (buf_len == 64) { process(buf); buf_len = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (buf_len != 56) update(&zero, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; ++i)
            lb[i] = static_cast<unsigned char>(bits >> (56 - 8*i));
        update(lb, 8);
        static const char* H = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 8; ++i)
            for (int s = 28; s >= 0; s -= 4)
                out += H[(h[i] >> s) & 0xF];
        return out;
    }
};

} // namespace detail

/** SHA-256 十六进制摘要 (审计用; 空串也有合法摘要) */
inline std::string sha256Hex(const std::string& data) {
    detail::Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

// ============================================================
// 文字单轮生命周期状态机 (主线程消费 WorkerEventQueue 后驱动)
// ============================================================

/** 退出码表（文档、测试和设备脚本保持一致） */
enum class TextTurnExit : int {
    Success          = 0,
    InvalidInput     = 2,
    WorkerTimeout    = 3,
    TurnError        = 4,
    Cancelled        = 5,
    TerminalTimeout  = 6,
    PageOrAuditFail  = 7,
};

/** 本类观测到的事件种类 (由 main_ai 从 AgentEvent 映射) */
enum class TextTurnEvent {
    State,             // text = state 值
    Content,           // text = 分片文本
    ResponseComplete,
    Expression,        // text = 情绪值
    TurnDone,
    TurnError,
    TurnCancelled,
    WorkerLost,
};

/** 情绪白名单 (与 response/interpreter.py 及 C++ Emotion 一致) */
inline bool isWhitelistedEmotion(const std::string& e) {
    return e == "neutral" || e == "happy" || e == "curious" ||
           e == "surprised" || e == "sad" || e == "sleepy" ||
           e == "concerned";
}

/** 生命周期阶段 */
enum class TextTurnPhase {
    WaitingWorker,
    Submitted,
    TurnActive,
    TerminalSuccess,
    TerminalError,
    HoldingPages,
    Finished,
};

class TextTurnLifecycle {
public:
    void configure(int64_t provider_wait_timeout_ms,
                   int64_t terminal_timeout_ms,
                   int64_t page_ms,
                   int64_t final_hold_ms) {
        provider_wait_timeout_ms_ = provider_wait_timeout_ms;
        terminal_timeout_ms_ = terminal_timeout_ms;
        page_ms_ = page_ms;
        final_hold_ms_ = final_hold_ms;
    }

    /** 提交成功 (IO 线程接受后由 main 填入真实 request_id) */
    void onSubmitted(const std::string& request_id, int64_t now_ms) {
        request_id_ = request_id;
        submit_ms_ = now_ms;
        if (phase_ == TextTurnPhase::WaitingWorker)
            phase_ = TextTurnPhase::Submitted;
    }

    /** 收到事件 (main 已按 request_id 过滤; 终态后的事件一律忽略) */
    void onEvent(TextTurnEvent kind, const std::string& text, int64_t now_ms) {
        if (phase_ == TextTurnPhase::TerminalSuccess ||
            phase_ == TextTurnPhase::TerminalError ||
            phase_ == TextTurnPhase::Finished) {
            return;   // 终态后的迟到/旧事件不得改变判定
        }
        if (phase_ == TextTurnPhase::Submitted) {
            phase_ = TextTurnPhase::TurnActive;   // 观察到本轮真实活动
            active_ms_ = now_ms;
        }
        switch (kind) {
            case TextTurnEvent::State:
                ++state_events_;
                break;
            case TextTurnEvent::Content:
                if (!text.empty()) {
                    ++content_chunks_;
                    content_bytes_ += text.size();
                }
                break;
            case TextTurnEvent::ResponseComplete:
                ++response_complete_;
                break;
            case TextTurnEvent::Expression:
                ++expression_;
                expression_value_ = text;
                if (!isWhitelistedEmotion(text)) expression_whitelisted_ = false;
                break;
            case TextTurnEvent::TurnDone:
                ++done_count_;
                done_ms_ = now_ms;
                break;
            case TextTurnEvent::TurnError:
                ++error_count_;
                error_ms_ = now_ms;
                break;
            case TextTurnEvent::TurnCancelled:
                ++cancel_count_;
                break;
            case TextTurnEvent::WorkerLost:
                ++worker_lost_;
                break;
        }
    }

    /** main 每帧把 ConversationController 的回答状态同步进来 */
    void setAnswerState(bool answer_valid, const std::string& committed) {
        answer_valid_ = answer_valid;
        committed_ = committed;
        committed_bytes_ = committed.size();
        committed_sha256_ = sha256Hex(committed);
    }

    /** response_complete 后由 main 绑定页数并开始展示计时 */
    void bindPages(int page_count, int64_t now_ms) {
        page_count_ = page_count;
        pages_.clear();
        if (page_count_ > 0) {
            PageShown p;
            p.index = 0; p.count = page_count; p.first_render_ms = now_ms;
            pages_.push_back(p);
            phase_ = TextTurnPhase::HoldingPages;
            hold_start_ms_ = now_ms;
        }
    }

    /** main 在实际进入渲染分支后调用 (记录"每页至少渲染一次") */
    void markPageRendered(int64_t now_ms) {
        if (pages_.empty()) return;
        PageShown& cur = pages_.back();
        cur.rendered = true;
        cur.last_render_ms = now_ms;
        if (!cur.rendered_first_ms_set) {
            cur.rendered_first_ms = now_ms;
            cur.rendered_first_ms_set = true;
        }
    }

    /** 当前页是否已满 dwell 且存在下一页 (终态后一律不翻页) */
    bool shouldAdvancePage(int64_t now_ms) const {
        if (phase_ != TextTurnPhase::HoldingPages) return false;
        if (pages_.empty() || page_count_ <= 1) return false;
        const PageShown& cur = pages_.back();
        if (!cur.rendered_first_ms_set) return false;
        if (cur.index + 1 >= page_count_) return false;   // 末页不翻
        return now_ms - cur.rendered_first_ms >= page_ms_;
    }

    /** 翻到下一页 (记录 page_shown 顺序; 终态后不再产生页面记录) */
    void advancePage(int64_t now_ms) {
        if (phase_ != TextTurnPhase::HoldingPages) return;
        if (pages_.empty()) return;
        pages_.back().advance_ms = now_ms;   // 用于计算页面停留时长
        PageShown p;
        p.index = pages_.back().index + 1;
        p.count = page_count_;
        p.first_render_ms = now_ms;
        pages_.push_back(p);
    }

    /**
     * R8_R2_R2 (P1-5): 单页实际停留时长 (毫秒)。
     *   非末页 = 翻页时刻 - 首次渲染时刻; 末页 = end_ms - 首次渲染时刻。
     * 未渲染或时间未记录时返回 0。
     */
    int64_t pageDwellMs(size_t idx, int64_t end_ms) const {
        if (idx >= pages_.size()) return 0;
        const PageShown& p = pages_[idx];
        if (!p.rendered_first_ms_set) return 0;
        const int64_t stop = (idx + 1 < pages_.size()) ? p.advance_ms : end_ms;
        if (stop <= 0 || stop < p.rendered_first_ms) return 0;
        return stop - p.rendered_first_ms;
    }

    /** 末页是否达到 final hold */
    bool finalHoldSatisfied(int64_t now_ms) const {
        if (pages_.empty()) return false;
        const PageShown& cur = pages_.back();
        if (cur.index + 1 != page_count_ || !cur.rendered_first_ms_set)
            return false;
        return now_ms - cur.rendered_first_ms >= final_hold_ms_;
    }

    /** 全部成功条件满足 (含末页 hold) → 允许完成 */
    bool readyToFinish(int64_t now_ms) const {
        if (error_count_ > 0 || cancel_count_ > 0 || worker_lost_ > 0)
            return false;
        if (request_id_.empty()) return false;
        // 必须在"正在展示真实回答页"这一阶段才可能成功:
        // WaitingWorker/Submitted/TurnActive = 尚无终态; Terminal* = 已失败。
        if (phase_ != TextTurnPhase::HoldingPages) return false;
        if (content_chunks_ < 1 || content_bytes_ == 0) return false;
        if (response_complete_ != 1) return false;
        if (expression_ != 1 || !expression_whitelisted_) return false;
        if (done_count_ != 1) return false;
        if (!answer_valid_ || committed_bytes_ == 0) return false;
        if (page_count_ < 1 || pages_.empty()) return false;
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].index != static_cast<int>(i)) return false;
            if (!pages_[i].rendered) return false;
        }
        if (pages_.back().index + 1 != page_count_) return false;
        if (!finalHoldSatisfied(now_ms)) return false;
        return true;
    }

    void markTerminalError() {
        if (phase_ != TextTurnPhase::Finished)
            phase_ = TextTurnPhase::TerminalError;
        exit_code_ = pickErrorExit();
    }
    void markCancelled() {
        if (phase_ != TextTurnPhase::Finished)
            phase_ = TextTurnPhase::TerminalError;
        exit_code_ = TextTurnExit::Cancelled;
    }
    void finishSuccess() {
        phase_ = TextTurnPhase::Finished;
        exit_code_ = TextTurnExit::Success;
    }
    void finishWith(TextTurnExit code) {
        phase_ = TextTurnPhase::Finished;
        exit_code_ = code;
    }

    /**
     * 超时检查: 返回需要立即退出的码, 或 -1 表示继续等待。
     * 展示等待 (HoldingPages) 只受页面 hold 控制, 不计入 Provider/终态超时。
     */
    int64_t pollTimeout(int64_t now_ms) const {
        if (phase_ == TextTurnPhase::WaitingWorker) {
            if (now_ms - start_ms_ > provider_wait_timeout_ms_)
                return static_cast<int64_t>(TextTurnExit::WorkerTimeout);
            return -1;
        }
        if (phase_ == TextTurnPhase::Submitted ||
            phase_ == TextTurnPhase::TurnActive) {
            if (now_ms - start_ms_ > terminal_timeout_ms_)
                return static_cast<int64_t>(TextTurnExit::TerminalTimeout);
        }
        return -1;
    }

    TextTurnExit pickErrorExit() const {
        if (cancel_count_ > 0) return TextTurnExit::Cancelled;
        return TextTurnExit::TurnError;
    }

    // ---- 观测/审计读取 ----
    TextTurnPhase phase() const { return phase_; }
    TextTurnExit exitCode() const { return exit_code_; }
    const std::string& requestId() const { return request_id_; }
    int contentChunks() const { return content_chunks_; }
    size_t contentBytes() const { return content_bytes_; }
    int responseCompleteCount() const { return response_complete_; }
    int expressionCount() const { return expression_; }
    const std::string& expressionValue() const { return expression_value_; }
    int doneCount() const { return done_count_; }
    int errorCount() const { return error_count_; }
    int cancelCount() const { return cancel_count_; }
    size_t committedBytes() const { return committed_bytes_; }
    const std::string& committedSha256() const { return committed_sha256_; }
    int pageCount() const { return page_count_; }

    struct PageShown {
        int index = 0;
        int count = 1;
        int64_t first_render_ms = 0;
        bool rendered = false;
        bool rendered_first_ms_set = false;
        int64_t rendered_first_ms = 0;
        int64_t last_render_ms = 0;
        int64_t advance_ms = 0;   // R8_R2_R2: 离开该页的时刻 (末页为 0)
    };
    const std::vector<PageShown>& pagesShown() const { return pages_; }

    void setStartMs(int64_t ms) { start_ms_ = ms; }

private:
    int64_t provider_wait_timeout_ms_ = 60000;
    int64_t terminal_timeout_ms_ = 60000;
    int64_t page_ms_ = 4000;
    int64_t final_hold_ms_ = 4000;

    TextTurnPhase phase_ = TextTurnPhase::WaitingWorker;
    TextTurnExit exit_code_ = TextTurnExit::TerminalTimeout;

    std::string request_id_;
    int64_t start_ms_ = 0;
    int64_t submit_ms_ = 0;
    int64_t active_ms_ = 0;
    int64_t done_ms_ = 0;
    int64_t error_ms_ = 0;
    int64_t hold_start_ms_ = 0;

    int state_events_ = 0;
    int content_chunks_ = 0;
    size_t content_bytes_ = 0;
    int response_complete_ = 0;
    int expression_ = 0;
    std::string expression_value_;
    bool expression_whitelisted_ = true;
    int done_count_ = 0;
    int error_count_ = 0;
    int cancel_count_ = 0;
    int worker_lost_ = 0;

    bool answer_valid_ = false;
    std::string committed_;
    size_t committed_bytes_ = 0;
    std::string committed_sha256_;

    int page_count_ = 0;
    std::vector<PageShown> pages_;
};

// ============================================================
// 事件审计 JSONL (原子: 先写 .tmp 再改名; 写失败 fail closed)
// ============================================================

class TextEventAudit {
public:
    /** 打开审计文件 (创建 .tmp); 失败返回 false → 调用方以 exit 7 退出 */
    bool open(const std::string& path) {
        path_ = path;
        tmp_path_ = path + ".tmp";
        std::remove(tmp_path_.c_str());
        f_.open(tmp_path_, std::ios::binary | std::ios::trunc);
        if (!f_) { failed_ = true; return false; }
        opened_ = true;
        return true;
    }
    bool writeLine(const std::string& json_line) {
        if (!opened_ || failed_) return false;
        f_ << json_line << "\n";
        if (!f_) { failed_ = true; return false; }
        return true;
    }
    /** 收尾: flush + close + 原子改名; 失败返回 false */
    bool commit() {
        if (!opened_ || failed_) { cleanupTmp(); return false; }
        f_.flush();
        f_.close();
        if (!f_) { failed_ = true; cleanupTmp(); return false; }
        if (std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
            failed_ = true; cleanupTmp(); return false;
        }
        opened_ = false;
        return true;
    }
    bool failed() const { return failed_; }

private:
    void cleanupTmp() {
        std::remove(tmp_path_.c_str());
        opened_ = false;
    }
    std::string path_, tmp_path_;
    std::ofstream f_;
    bool opened_ = false;
    bool failed_ = false;
};

} // namespace holopet
