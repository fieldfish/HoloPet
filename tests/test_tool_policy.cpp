/**
 * test_tool_policy.cpp — V6 工具白名单测试
 */

#include "agent/tool_policy.hpp"
#include <iostream>
#include <string>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_tool_policy (V6) ===\n\n";

    check(ToolPolicy::isAllowed("get_time"), "白名单 get_time");
    check(ToolPolicy::isAllowed("get_status"), "白名单 get_status");
    check(ToolPolicy::isAllowed("set_volume"), "白名单 set_volume");
    check(ToolPolicy::isAllowed("set_expression"), "白名单 set_expression");

    check(!ToolPolicy::isAllowed("run_shell"), "未注册工具拒绝");
    check(!ToolPolicy::isAllowed("read_file"), "未注册 read_file 拒绝");
    check(!ToolPolicy::isAllowed(""), "空名拒绝");

    check(ToolPolicy::isDangerous("shell"), "shell 危险");
    check(ToolPolicy::isDangerous("exec_command"), "exec 危险");
    check(ToolPolicy::isDangerous("subprocess_run"), "subprocess 危险");
    check(ToolPolicy::isDangerous("os.system"), "os. 危险");
    check(ToolPolicy::isDangerous("import_module"), "import 危险");
    check(ToolPolicy::isDangerous("__builtins__"), "__ 危险");
    check(!ToolPolicy::isDangerous("get_time"), "get_time 安全");

    check(ToolPolicy::clampToolRounds(4) == 4, "轮次 4 保留");
    check(ToolPolicy::clampToolRounds(100) == 16, "轮次上限 16");
    check(ToolPolicy::clampToolRounds(-3) == 0, "轮次下限 0");

    // 白名单里不存在危险名 (自洽)
    bool self_ok = true;
    for (const auto& t : ToolPolicy::allowedTools()) {
        if (ToolPolicy::isDangerous(t)) self_ok = false;
    }
    check(self_ok, "白名单与危险名单不冲突");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
