#pragma once
/**
 * worker_args.hpp — R4-R2 (F1): --worker-* 参数解析 (可单测)
 *
 * 修正 R4-R1 缺陷: 单一标志同时记录 uds/host/port, 导致只传 --worker-uds
 * 也命中冲突检查 (systemd 生产入口直接退出)。
 * 规则:
 *   - --worker-uds 单独出现 → 允许 (UDS 配置)
 *   - --worker-host/--worker-port 单独或同时出现 → 允许 (TCP 配置)
 *   - UDS 与显式 host/port 同时出现 → fail closed (error 非空)
 *   - 缺值 / 端口非数字 → error
 * 平台支持检查由调用方负责 (Windows 无 AF_UNIX: 显式 UDS 报"不支持",
 * 不静默改 TCP)。
 */

#include "agent/agent_config.hpp"

#include <cstdlib>
#include <string>

namespace holopet {

struct ParsedWorkerArgs {
    std::string host = "127.0.0.1";
    int port = 47650;
    std::string uds_path;
    bool host_given = false;
    bool port_given = false;
    bool uds_given = false;
    std::string error;   // 非空 = 参数拒绝 (调用方必须退出)

    /** 应用到配置 (error 为空时才调用) */
    void applyTo(AiWorkerConfig& cfg) const {
        if (host_given) cfg.host = host;
        if (port_given) cfg.port = port;
        if (uds_given) cfg.uds_path = uds_path;
    }
};

/** 解析 argv 中的 --worker-* 参数; 其余参数忽略。永不抛异常。 */
inline ParsedWorkerArgs parseWorkerArgs(int argc, char* argv[]) {
    ParsedWorkerArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto needValue = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                a.error = flag + " 缺少参数值";
                return "";
            }
            return argv[++i];
        };
        if (s == "--worker-host") {
            std::string v = needValue(s);
            if (!a.error.empty()) return a;
            a.host = v;
            a.host_given = true;
        } else if (s == "--worker-port") {
            std::string v = needValue(s);
            if (!a.error.empty()) return a;
            char* end = nullptr;
            long p = std::strtol(v.c_str(), &end, 10);
            if (!end || *end != '\0' || p <= 0 || p > 65535) {
                a.error = "--worker-port 端口非法: " + v;
                return a;
            }
            a.port = static_cast<int>(p);
            a.port_given = true;
        } else if (s == "--worker-uds") {
            std::string v = needValue(s);
            if (!a.error.empty()) return a;
            if (v.empty()) {
                a.error = "--worker-uds 路径为空";
                return a;
            }
            a.uds_path = v;
            a.uds_given = true;
        }
    }
    // R4-R2 (F1): 仅 UDS+显式 host/port 同时出现才冲突
    if (a.uds_given && (a.host_given || a.port_given)) {
        a.error = "--worker-uds conflicts with --worker-host/--worker-port (互斥); 请只选一种传输";
    }
    return a;
}

} // namespace holopet
