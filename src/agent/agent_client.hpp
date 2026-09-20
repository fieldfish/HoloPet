#pragma once
/**
 * agent_client.hpp — AI Worker 会话 (跨平台 TCP 客户端)  V6-R2
 *
 * R2 socket 正确性:
 *  - 非阻塞 connect → select 可写 + SO_ERROR 确认成功 (EINPROGRESS 不报告 connected)
 *  - 连接成功后等待 worker "hello" 握手才置 Connected
 *  - recv()==0 → peer EOF: 关 socket、清收发缓冲与 active request、只发一次 WorkerLost
 *  - sendLine 处理 partial send / EWOULDBLOCK / EINTR; 待发数据入有上限缓冲, 不静默丢失
 *  - 重连 backoff 有上限; 重连后重新握手, 不使用旧 request_id
 *  - 本类只由 IO owner 线程调用 (SDL 主线程通过 AiCommandQueue 间接驱动)
 */

#include "agent/agent_config.hpp"
#include "ipc/ai_worker_client.hpp"
#include "ipc/ai_worker_protocol.hpp"
#include "system/logger.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#endif

namespace holopet {

namespace {
inline int sockErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
inline bool sockWouldBlock() {
#ifdef _WIN32
    return sockErr() == WSAEWOULDBLOCK;
#else
    return sockErr() == EAGAIN || sockErr() == EWOULDBLOCK || sockErr() == EINTR;
#endif
}
} // namespace

/** 跨平台 TCP socket RAII (非阻塞) */
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { closeSocket(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool valid() const noexcept {
#ifdef _WIN32
        return fd_ != INVALID_SOCKET;
#else
        return fd_ >= 0;
#endif
    }

    /** 开始非阻塞 connect (只允许回环); 返回 false = 立即失败 */
    bool connectLoopback(const std::string& host, int port) {
        closeSocket();
#ifdef _WIN32
        WSADATA wsa;
        static bool wsa_ok = false;
        if (!wsa_ok) {
            wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
            if (!wsa_ok) return false;
        }
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == INVALID_SOCKET) return false;
        u_long mode = 1;
        ioctlsocket(fd_, FIONBIO, &mode);
#else
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        if (host == "127.0.0.1" || host == "localhost") {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            closeSocket();
            return false;
        }
        if ((ntohl(addr.sin_addr.s_addr) >> 24) != 127) {
            LOG_WARN("IPC") << "refusing non-loopback worker host: " << host;
            closeSocket();
            return false;
        }
#ifdef _WIN32
        int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS && e != WSAEINVAL) {
                closeSocket();
                return false;
            }
        }
#else
        int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (r < 0 && errno != EINPROGRESS) {
            closeSocket();
            return false;
        }
#endif
        return true;
    }

    /**
     * R4-R1 (D9): Unix Domain Socket 非阻塞 connect (Linux 生产路径)。
     * 与 connectLoopback 走同一 SocketConn/握手/发送缓冲/request filter。
     * Windows 无 AF_UNIX → 返回 false (开发回退 loopback TCP)。
     */
    bool connectUnix(const std::string& uds_path) {
        closeSocket();
#ifdef _WIN32
        (void)uds_path;
        return false;
#else
        if (uds_path.empty()) return false;
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (uds_path.size() >= sizeof(addr.sun_path)) {
            closeSocket();
            return false;
        }
        std::memcpy(addr.sun_path, uds_path.c_str(), uds_path.size() + 1);
        int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                          static_cast<socklen_t>(sizeof(addr)));
        if (r < 0 && errno != EINPROGRESS) {
            closeSocket();
            return false;
        }
        return true;
#endif
    }

    /**
     * 检查非阻塞 connect 是否完成 (select 可写 + SO_ERROR==0)。
     * 返回: 1=成功, 0=进行中, -1=失败
     */
    int checkConnect() {
        if (!valid()) return -1;
        fd_set wf;
        FD_ZERO(&wf);
#ifdef _WIN32
        FD_SET(fd_, &wf);
#else
        FD_SET(fd_, &wf);
#endif
        timeval tv{0, 0};
        int sel = select(static_cast<int>(fd_) + 1, nullptr, &wf, nullptr, &tv);
        if (sel <= 0) return 0;
        int so_err = 0;
#ifdef _WIN32
        int len = sizeof(so_err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len);
#else
        socklen_t len = sizeof(so_err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_err, &len);
#endif
        return (so_err == 0) ? 1 : -1;
    }

    /**
     * 发送一行 (处理 partial send / EWOULDBLOCK / EINTR)。
     * 返回: 1=全部发送, 0=部分发送 (剩余由调用方缓冲重试), -1=失败(断开)
     * 输出 sent = 已发送字节数。
     */
    int sendSome(const char* data, size_t len, size_t& sent) {
        sent = 0;
        if (!valid()) return -1;
        while (sent < len) {
#ifdef _WIN32
            int n = ::send(fd_, data + sent, static_cast<int>(len - sent), 0);
#else
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
#endif
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && sockWouldBlock()) return (sent > 0) ? 0 : 0;   // 无进展
            return -1;
        }
        return 1;
    }

    /** 非阻塞读取; 返回: >0 字节数, 0=无数据(EWOULDBLOCK), -1=失败(EOF/错误) */
    int recvSome(char* buf, size_t max) {
        if (!valid()) return -1;
#ifdef _WIN32
        int n = ::recv(fd_, buf, static_cast<int>(max), 0);
        if (n == 0) return -1;                       // peer EOF
        if (n == SOCKET_ERROR) {
            return sockWouldBlock() ? 0 : -1;
        }
#else
        ssize_t n = ::recv(fd_, buf, max, 0);
        if (n == 0) return -1;                       // peer EOF
        if (n < 0) return sockWouldBlock() ? 0 : -1;
#endif
        return static_cast<int>(n);
    }

    void closeSocket() noexcept {
#ifdef _WIN32
        if (fd_ != INVALID_SOCKET) { closesocket(fd_); fd_ = INVALID_SOCKET; }
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }

private:
#ifdef _WIN32
    SOCKET fd_ = INVALID_SOCKET;
#else
    int fd_ = -1;
#endif
};

/** 会话阶段 */
enum class SessionPhase {
    Disconnected,     // 等待重连 backoff
    Connecting,       // 非阻塞 connect 进行中
    AwaitHandshake,   // TCP 已连, 等待 worker "hello"
    Connected         // 握手完成, 可开始 turn
};

/**
 * AI Worker 会话 (唯一 IO owner)。
 * 状态快照仅供渲染线程读取; 可变成员只由 IO 线程访问。
 */
class AiWorkerSession {
public:
    struct Status {
        bool    connected = false;
        int64_t last_change_ms = 0;
    };

    explicit AiWorkerSession(const AiWorkerConfig& cfg = AiWorkerConfig{})
        : cfg_(cfg), next_backoff_ms_(cfg.reconnect_interval_ms) {}

    /** 线程安全状态快照 (渲染线程用) */
    Status status() const {
        std::lock_guard<std::mutex> lk(status_mtx_);
        return status_;
    }

    /**
     * IO 线程每 tick 调用:
     *  - 状态机推进 (重连/connect 确认/握手/读取/发送 flush)
     *  - 事件进入 out 队列 (WorkerLost 每个断线只发一次)
     */
    void drive(int64_t now_ms, WorkerEventQueue& out) {
        switch (phase_) {
        case SessionPhase::Disconnected: {
            if (now_ms - last_attempt_ms_ < next_backoff_ms_) return;
            last_attempt_ms_ = now_ms;
            // R4-R1 (D9): 生产 UDS 优先; Windows 开发回退 loopback TCP
            const bool connecting = !cfg_.uds_path.empty()
                ? sock_.connectUnix(cfg_.uds_path)
                : sock_.connectLoopback(cfg_.host, cfg_.port);
            if (connecting) {
                phase_ = SessionPhase::Connecting;
            }
            break;
        }

        case SessionPhase::Connecting: {
            int r = sock_.checkConnect();
            if (r == 1) {
                phase_ = SessionPhase::AwaitHandshake;
                handshake_deadline_ms_ = now_ms + kHandshakeTimeoutMs;
                pending_.clear();          // 旧连接残留不得发往新连接
                lines_.clear();
                filter_.clear();
                active_id_.clear();
            } else if (r == -1) {
                dropConnection(now_ms, out, false);
            }
            break;
        }

        case SessionPhase::AwaitHandshake:
        case SessionPhase::Connected: {
            // 1) 接收
            char buf[2048];
            for (;;) {
                int n = sock_.recvSome(buf, sizeof(buf));
                if (n == 0) break;                       // 无数据
                if (n < 0) {                             // EOF / 错误
                    bool was_connected = (phase_ == SessionPhase::Connected);
                    dropConnection(now_ms, out, was_connected);
                    return;
                }
                if (lines_.append(buf, static_cast<size_t>(n))) {
                    // R4: 无换行部分包超限 → 缓冲已清空, 丢弃并继续
                    ++protocol_error_count_;
                    continue;
                }
                bool got_hello = false;
                for (auto line = lines_.popLine(); line; line = lines_.popLine()) {
                    auto res = parseWorkerLine(*line);
                    if (res.error) {
                        ++protocol_error_count_;
                        continue;                          // 结构错误: 丢弃, 不崩溃
                    }
                    if (!res.message) continue;
                    const WorkerMessage& m = *res.message;
                    if (phase_ == SessionPhase::AwaitHandshake) {
                        if (m.type == "hello") {
                            phase_ = SessionPhase::Connected;
                            got_hello = true;
                            setStatus(true, now_ms);
                            LOG_INFO("IPC") << "handshake complete (connected)";
                            out.push(AgentEvent{AgentEventType::WorkerOnline});
                        }
                        continue;                          // 握手期其他消息忽略
                    }
                    if (!filter_.accept(m)) {
                        ++stale_dropped_count_;            // 过期 request_id 丢弃计数
                        continue;
                    }
                    if (auto ev = toAgentEvent(m)) {
                        out.push(*ev);
                        if (ev->type == AgentEventType::TurnDone ||
                            ev->type == AgentEventType::TurnError ||
                            ev->type == AgentEventType::TurnCancelled) {
                            filter_.clear();
                            active_id_.clear();
                        }
                    }
                }
                (void)got_hello;
            }
            // 2) 握手超时
            if (phase_ == SessionPhase::AwaitHandshake &&
                now_ms > handshake_deadline_ms_) {
                dropConnection(now_ms, out, false);
                return;
            }
            // 3) 发送缓冲 flush (partial/EWOULDBLOCK 重试)
            flushPending(now_ms, out);
            break;
        }
        }
    }

    /** 开始新一轮 (IO 线程): 生成 request_id 并入发送缓冲 */
    std::string startTurn(const std::string& ai_mode = "auto") {
        // R4-R1 (D9 测试接缝): 允许固定下一轮 rid (transcript 需同 rid)
        active_id_ = next_test_rid_.empty() ? makeRequestId() : next_test_rid_;
        next_test_rid_.clear();
        filter_.setActive(active_id_);
        // R8_R4_R3_R4: 每轮携带 AI 模式 (菜单选择 → router 真实绑定)
        pending_.push_back(buildOutMessage(active_id_, "start_turn",
                                           "ai_mode", ai_mode));
        return active_id_;
    }

    /** R4-R1 (D9 测试接缝): 注入 transcript (同一发送缓冲/过滤路径)。
     * 生产路径由 EC11 触发; 测试用它在 start_turn 前预置听写文本。 */
    void queueTranscriptForTest(const std::string& rid, const std::string& text) {
        pending_.push_back(buildOutMessage(rid, "transcript", "text", text));
    }
    /** 测试接缝: 固定下一轮 request_id (与 queueTranscriptForTest 配套) */
    void setNextRequestIdForTest(const std::string& rid) { next_test_rid_ = rid; }

    /** 取消当前轮 (幂等) */
    void cancelTurn() {
        if (active_id_.empty()) return;
        pending_.push_back(buildOutMessage(active_id_, "cancel"));
    }

    /** R8_R4: 发送原始 JSONL 行 (tool_result/feature_event 回复);
     *  仅 IO owner 线程调用 (经 AiCommand::SendRaw 投递)。 */
    void sendRawLine(const std::string& line) {
        if (line.empty()) return;
        // R8_R4_R2: JSONL 契约 — 原始行必须带换行符终止, 否则对端
        // 读取器永不分行 (Pi 实测 tool_timeout 根因之一: tool_result
        // 行无换行, agentd 等不到完整行)。
        if (line.back() == '\n') pending_.push_back(line);
        else pending_.push_back(line + "\n");
    }

    /** 请求提前结束录音 */
    void stopRecording() {
        if (active_id_.empty()) return;
        pending_.push_back(buildOutMessage(active_id_, "stop_recording"));
    }

    /**
     * R8_R2 (工作包 D): 文字输入发起一轮 — 仅 IO owner 线程调用。
     * 同一 request_id 下按协议序发送 transcript → start_turn →
     * stop_recording。以下情况返回 false 且不伪造成功: 空文本 /
     * 未连接 / 已有 active turn (busy, 受控拒绝不交错)。
     */
    bool submitTextTurn(const std::string& text,
                        const std::string& ai_mode = "auto") {
        if (text.empty()) return false;
        if (!status().connected) return false;
        if (!active_id_.empty()) return false;      // busy
        const std::string rid = makeRequestId();
        active_id_ = rid;
        filter_.setActive(rid);
        {
            // R8_R2_R1 (A): 供主线程观测本轮真实 request_id (审计/判定)
            std::lock_guard<std::mutex> lk(rid_mtx_);
            last_text_rid_ = rid;
        }
        pending_.push_back(buildOutMessage(rid, "transcript", "text", text));
        pending_.push_back(buildOutMessage(rid, "start_turn",
                                           "ai_mode", ai_mode));
        pending_.push_back(buildOutMessage(rid, "stop_recording"));
        return true;
    }

    /** R8_R2_R1 (A): 最近一次文字轮的 request_id (空 = 尚未提交)。
     *  IO 线程写 / 主线程读, 由 rid_mtx_ 保护。 */
    std::string textTurnRequestId() const {
        std::lock_guard<std::mutex> lk(rid_mtx_);
        return last_text_rid_;
    }

    /** 退出: 关 socket, 清状态 */
    void shutdown() {
        sock_.closeSocket();
        filter_.clear();
        active_id_.clear();
        pending_.clear();
        lines_.clear();
        phase_ = SessionPhase::Disconnected;
        next_backoff_ms_ = cfg_.reconnect_interval_ms;
        setStatus(false, 0);
    }

    /** 调试计数 (测试/日志用) */
    int64_t staleDropped() const { return stale_dropped_count_; }
    int64_t protocolErrors() const { return protocol_error_count_; }

    /** 无第三方 UUID */
    static std::string makeRequestId() {
        static std::uint64_t counter = 0;
        ++counter;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "turn-" + std::to_string(static_cast<std::uint64_t>(now)) +
               "-" + std::to_string(counter);
    }

private:
    static constexpr int64_t kHandshakeTimeoutMs = 3000;
    static constexpr size_t  kMaxPending = 64;

    void dropConnection(int64_t now_ms, WorkerEventQueue& out, bool was_connected) {
        sock_.closeSocket();
        filter_.clear();
        active_id_.clear();
        pending_.clear();
        lines_.clear();
        phase_ = SessionPhase::Disconnected;
        // backoff: 有上限
        next_backoff_ms_ = (std::min)(next_backoff_ms_ * 2,
                                      static_cast<int64_t>(15000));
        setStatus(false, now_ms);
        if (was_connected) {
            out.push(AgentEvent{AgentEventType::WorkerLost});   // 每次断开只发一次
        }
    }

    void setStatus(bool connected, int64_t now_ms) {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_.connected = connected;
        status_.last_change_ms = now_ms;
    }

    void flushPending(int64_t now_ms, WorkerEventQueue& out) {
        while (!pending_.empty()) {
            const std::string& line = pending_.front();
            size_t sent = 0;
            int r = sock_.sendSome(line.data(), line.size(), sent);
            if (r < 0) {
                dropConnection(now_ms, out, true);
                return;
            }
            if (sent == line.size()) {
                pending_.pop_front();
            } else if (sent > 0) {
                // 部分发送: 保留剩余
                pending_.front() = line.substr(sent);
                break;
            } else {
                break;   // EWOULDBLOCK, 下轮重试
            }
        }
        if (pending_.size() > kMaxPending) {
            LOG_WARN("IPC") << "send buffer overflow, dropping old commands";
            while (pending_.size() > kMaxPending) pending_.pop_front();
        }
    }

    AiWorkerConfig cfg_;
    TcpSocket sock_;
    SessionPhase phase_ = SessionPhase::Disconnected;
    LineBuffer lines_;
    RequestIdFilter filter_;
    std::string active_id_;
    std::string next_test_rid_;   // 测试接缝
    mutable std::mutex rid_mtx_;  // R8_R2_R1: last_text_rid_ 保护
    std::string last_text_rid_;
    std::deque<std::string> pending_;

    int64_t last_attempt_ms_ = -1 << 30;
    int64_t next_backoff_ms_ = 0;          // 初始立即尝试
    int64_t handshake_deadline_ms_ = 0;
    int64_t stale_dropped_count_ = 0;
    int64_t protocol_error_count_ = 0;

    mutable std::mutex status_mtx_;
    Status status_;
};

} // namespace holopet
