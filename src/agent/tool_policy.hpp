#pragma once
/**
 * tool_policy.hpp — Agent 工具白名单策略 (纯 C++, 可单测)  V6
 *
 * 所有工具静态注册并进入显式白名单; 禁止 shell/exec 类工具。
 */

#include <algorithm>
#include <string>
#include <vector>

namespace holopet {

class ToolPolicy {
public:
    /** 注册白名单 (静态, 唯一来源) */
    static const std::vector<std::string>& allowedTools() {
        static const std::vector<std::string> kAllowed = {
            "get_time",        // 获取当前时间
            "get_status",      // 查询设备状态 (音量/情绪/电量候选)
            "set_volume",      // 设置音量 0..1
            "set_expression",  // 设置情绪表情
        };
        return kAllowed;
    }

    /** 白名单判定 */
    static bool isAllowed(const std::string& name) {
        const auto& list = allowedTools();
        return std::find(list.begin(), list.end(), name) != list.end();
    }

    /**
     * 危险工具判定 (即使白名单意外放行也拒绝)
     * 覆盖 shell/exec/subprocess/system 类名称模式
     */
    static bool isDangerous(const std::string& name) {
        static const char* kPatterns[] = {
            "shell", "exec", "subprocess", "system", "popen",
            "eval", "os.", "import", "__"
        };
        for (const char* p : kPatterns) {
            if (name.find(p) != std::string::npos) return true;
        }
        return false;
    }

    /** 工具轮次上限 (配置值夹取到 [0,16]) */
    static int clampToolRounds(int configured) {
        return std::max(0, std::min(configured, 16));
    }
};

} // namespace holopet
