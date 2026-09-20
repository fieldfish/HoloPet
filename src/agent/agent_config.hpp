#pragma once
/**
 * agent_config.hpp — Agent / Worker 配置 (纯 C++, 可单测)  V6
 *
 * C++ 侧保存 Worker 连接与开关; Provider 细节 (base_url/model/超时) 由
 * ai_worker 的 JSON 配置消费, 此处镜像其字段以便校验与文档一致。
 * API Key 只能从环境变量读取 (api_key_env_name), 禁止在配置里出现真实密钥。
 */

#include <string>

namespace holopet {

/** Provider 细节 (镜像 config/agent.example.json) */
struct AgentConfig {
    std::string provider            = "fake";   // fake | openai_compatible
    std::string base_url            = "";       // openai_compatible: https://...
    std::string model               = "holopet-default";
    std::string api_key_env_name    = "HOLOPET_API_KEY";
    int         connect_timeout_ms  = 5000;
    int         request_timeout_ms  = 30000;
    int         max_output_tokens   = 256;
    int         max_history_turns   = 8;
    int         max_tool_rounds     = 4;
    std::string voice               = "default";   // TTS 声音 (provider 定义)
    std::string language            = "zh";        // 语言候选
};

/** C++ 主程序 ↔ AI Worker IPC 配置 */
struct AiWorkerConfig {
    std::string host   = "127.0.0.1";   // 只允许本机回环
    int         port   = 47650;
    std::string uds_path = "";          // R4-R1 (D9): Linux 生产
                                        // /run/holopet/agent.sock;
                                        // 非空 → UDS 优先 (Windows 忽略)
    int         reconnect_interval_ms = 1000;
    std::string launch_cmd = "";        // 非空 = 由主程序 spawn (Windows/dev);
                                        // 空 = 由 systemd 独立管理 (Pi 生产)
    bool        spawn_on_start = false;
};

} // namespace holopet
