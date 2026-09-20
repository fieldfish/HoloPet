/**
 * test_worker_args.cpp — R4-R2 (F1): --worker-* 参数解析单测
 *
 * 覆盖命令行参数的最低测试用例:
 *  UDS only=允许 / host only=允许 / port only=允许 / 主机+端口=允许 /
 *  UDS+host=拒绝 / UDS+port=拒绝 / 缺少值=拒绝 / 端口非法=拒绝
 */

#include "ipc/worker_args.hpp"

#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

static ParsedWorkerArgs parse(std::initializer_list<const char*> args) {
    std::vector<std::string> store{"/path/holopet_ai"};
    std::vector<char*> argv;
    for (const char* a : args) store.emplace_back(a);
    for (auto& s : store) argv.push_back(const_cast<char*>(s.c_str()));
    return parseWorkerArgs(static_cast<int>(argv.size()), argv.data());
}

int main() {
    std::cout << "=== test_worker_args (V6 R4-R2) ===\n\n";

    {
        auto a = parse({"--worker-uds", "/run/holopet/agent.sock"});
        check(a.error.empty(), "UDS only 允许 (无冲突误报)");
        check(a.uds_given && a.uds_path == "/run/holopet/agent.sock",
              "UDS only 进入 UDS 配置");
        check(!a.host_given && !a.port_given, "UDS only 不动 host/port");
    }
    {
        auto a = parse({"--worker-host", "127.0.0.1"});
        check(a.error.empty() && a.host_given && !a.uds_given, "host only 允许");
    }
    {
        auto a = parse({"--worker-port", "5000"});
        check(a.error.empty() && a.port_given && a.port == 5000, "port only 允许");
    }
    {
        auto a = parse({"--worker-host", "127.0.0.1", "--worker-port", "5000"});
        check(a.error.empty() && a.host_given && a.port_given,
              "host+port 同时允许 (TCP)");
    }
    {
        auto a = parse({"--worker-uds", "/tmp/s.sock",
                        "--worker-host", "127.0.0.1"});
        check(!a.error.empty(), "UDS+host 拒绝");
    }
    {
        auto a = parse({"--worker-uds", "/tmp/s.sock", "--worker-port", "5000"});
        check(!a.error.empty(), "UDS+port 拒绝");
    }
    {
        auto a = parse({"--worker-uds"});
        check(!a.error.empty(), "UDS 缺值拒绝");
        auto b = parse({"--worker-host"});
        check(!b.error.empty(), "host 缺值拒绝");
        auto c2 = parse({"--worker-port"});
        check(!c2.error.empty(), "port 缺值拒绝");
    }
    {
        auto a = parse({"--worker-port", "not-a-number"});
        check(!a.error.empty(), "端口非数字拒绝");
        auto b = parse({"--worker-port", "99999"});
        check(!b.error.empty(), "端口超范围拒绝");
    }
    {
        // 非 worker 参数不影响解析 (混合 argv)
        auto a = parse({"--fullscreen", "--worker-uds", "/tmp/s.sock",
                        "--profile-file", "p.json"});
        check(a.error.empty() && a.uds_given, "混合 argv 只取 --worker-*");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
