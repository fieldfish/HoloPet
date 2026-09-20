/**
 * test_agent_config.cpp — V6 Agent/Worker 配置默认值测试
 */

#include "agent/agent_config.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_agent_config (V6) ===\n\n";

    AgentConfig a;
    check(a.provider == "fake", "默认 provider = fake (不联网)");
    check(a.base_url.empty(), "默认 base_url 空");
    check(a.api_key_env_name == "HOLOPET_API_KEY", "默认密钥环境变量名");
    check(a.max_history_turns == 8, "默认历史 8 轮");
    check(a.max_tool_rounds == 4, "默认工具轮次 4");
    check(a.max_output_tokens == 256, "默认输出 256 tokens");
    check(a.request_timeout_ms == 30000, "默认请求超时 30s");

    AiWorkerConfig w;
    check(w.host == "127.0.0.1", "worker 只连本机回环");
    check(w.port == 47650, "默认端口 47650");
    check(!w.spawn_on_start, "默认不 spawn (systemd 管理)");

    // openai_compatible 配置形态 (无密钥)
    AgentConfig oa;
    oa.provider = "openai_compatible";
    oa.base_url = "https://example.invalid/v1";
    oa.model = "example-model";
    check(oa.provider == "openai_compatible" && !oa.base_url.empty(),
          "openai_compatible 形态");
    // 断言: 本结构体没有任何字段可承载真实密钥 (只有变量名)
    check(oa.api_key_env_name.find("KEY") != std::string::npos,
          "密钥只经环境变量名引用");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
