/**
 * test_worker_session.cpp — AiWorkerSession socket 语义集成测试 (V6-R2)
 * 本地回环: hello 握手 / EOF→WorkerLost 一次 / 过期丢弃计数 / 重连握手。
 */

#include "agent/agent_client.hpp"
#include <atomic>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

namespace {

class TestServer {
public:
    bool start(int port) {
        stop();   // 重开前先关旧 fd (避免旧 listener 吞掉新连接)
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int on = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(fd_, 2) != 0) return false;
        return true;
    }
    /** accept (带 3s 超时, 避免测试挂起); 超时返回 -2 */
    int acceptOne(int timeout_sec = 8) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd_, &rf);
        timeval tv{timeout_sec, 0};
        int sel = select(static_cast<int>(fd_) + 1, &rf, nullptr, nullptr, &tv);
        if (sel <= 0) return -2;
        sockaddr_in peer{};
#ifdef _WIN32
        int len = sizeof(peer);
#else
        socklen_t len = sizeof(peer);
#endif
        return ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &len);
    }
    void stop() {
        if (fd_ >= 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }
    ~TestServer() { stop(); }

    static bool sendAll(int cfd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            int n = ::send(cfd, data.data() + off, static_cast<int>(data.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    static void closeFd(int cfd) {
#ifdef _WIN32
        closesocket(cfd);
#else
        ::close(cfd);
#endif
    }

private:
#ifdef _WIN32
    SOCKET fd_ = INVALID_SOCKET;
#else
    int fd_ = -1;
#endif
};

int64_t fakeNow() {
    static std::atomic<int64_t> t{0};
    return t.fetch_add(10);   // 10ms 步进 (backoff 200ms 在 30 次 drive 内可达)
}

void driveN(AiWorkerSession& s, WorkerEventQueue& q, int n) {
    for (int i = 0; i < n; ++i) s.drive(fakeNow(), q);
}

} // namespace

int main() {
    std::cout << "=== test_worker_session (V6-R2) ===\n\n";

    // ---- 1. 握手: hello → connected + WorkerOnline ----
    {
        TestServer srv;
        check(srv.start(47651), "listener 47651 启动");
        AiWorkerConfig cfg; cfg.port = 47651; cfg.reconnect_interval_ms = 100;
        AiWorkerSession s(cfg);
        WorkerEventQueue q;
        driveN(s, q, 20);                       // 连接 + connect 确认
        int cfd = srv.acceptOne();
        check(cfd >= 0, "accept 客户端");
        TestServer::sendAll(cfd, "{\"v\":\"1\",\"request_id\":\"\",\"type\":\"hello\"}\n");
        driveN(s, q, 20);                       // 读 hello → 握手完成
        check(s.status().connected, "hello 后 connected=true");
        AgentEvent ev;
        bool got_online = false;
        while (q.tryPop(ev)) if (ev.type == AgentEventType::WorkerOnline) got_online = true;
        check(got_online, "WorkerOnline 事件一次");

        // ---- 2. EOF → WorkerLost 只发一次 ----
        TestServer::closeFd(cfd);
        driveN(s, q, 20);                       // recv EOF → drop
        check(!s.status().connected, "EOF 后 connected=false");
        int lost_count = 0;
        while (q.tryPop(ev)) if (ev.type == AgentEventType::WorkerLost) ++lost_count;
        check(lost_count == 1, "WorkerLost 只发一次");
        driveN(s, q, 20);                       // 断线期不重复
        lost_count = 0;
        while (q.tryPop(ev)) if (ev.type == AgentEventType::WorkerLost) ++lost_count;
        check(lost_count == 0, "断线期不重复发 WorkerLost");

        // ---- 3. 重连 + 重新握手 ----
        check(srv.start(47651), "listener 重启");
        driveN(s, q, 300);                      // backoff (上限 15s) 内重连
        int cfd2 = srv.acceptOne();
        check(cfd2 >= 0, "重连 accept");
        check(!s.status().connected, "重连后 (握手前) 不报 connected");
        TestServer::sendAll(cfd2, "{\"v\":\"1\",\"request_id\":\"\",\"type\":\"hello\"}\n");
        driveN(s, q, 20);
        check(s.status().connected, "重连后重新握手成功");

        // ---- 4. 过期 request_id 丢弃 ----
        std::string rid = s.startTurn();
        check(!rid.empty(), "startTurn 生成 rid");
        TestServer::sendAll(cfd2, "{\"v\":\"1\",\"request_id\":\"old-id\",\"type\":\"transcript\",\"text\":\"过期\"}\n");
        int64_t before = s.staleDropped();
        driveN(s, q, 20);
        check(s.staleDropped() > before, "过期 request_id 丢弃并计数");
        TestServer::closeFd(cfd2);
    }

    // ---- 5. 协议坏行不崩溃 ----
    {
        TestServer srv;
        check(srv.start(47652), "listener 47652 启动");
        AiWorkerConfig cfg; cfg.port = 47652; cfg.reconnect_interval_ms = 100;
        AiWorkerSession s(cfg);
        WorkerEventQueue q;
        driveN(s, q, 20);
        int cfd = srv.acceptOne();
        TestServer::sendAll(cfd, "{\"v\":\"1\",\"request_id\":\"\",\"type\":\"hello\"}\n");
        driveN(s, q, 20);
        check(s.status().connected, "握手完成");
        TestServer::sendAll(cfd, "{bad json\n");
        int64_t pe_before = s.protocolErrors();
        driveN(s, q, 20);
        check(s.protocolErrors() > pe_before, "坏 JSON → 结构化错误计数, 不崩溃");
        check(s.status().connected, "坏行不影响连接");
        TestServer::closeFd(cfd);
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
