#pragma once
/**
 * gpio_config.hpp — GPIO 引脚集中配置 (V2-R1 → V6 field fixes → R7)
 *
 * GPIO 编号不得散落在多个源码文件。
 *
 * V6 现场差异合入 (Raspberry Pi 5 实测):
 *  - 默认 A=27 / B=17 / SW=22 — 锁定现场真实旋钮方向 (原 A17/B27 为初版猜测)
 *  - chip_path: 手工 override (空 = 自动探测)
 *  - validate(): 生产代码自检
 *
 * R7 (B1): 不再硬编码 /dev/gpiochip4 — Pi 5 以 controller 标签
 * "pinctrl-rp1" 为稳定身份解析 (数字索引随内核/设备树变化)。
 * resolveGpioChipPath() 为三入口与单测共享的纯解析逻辑。
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace holopet {

struct GpioConfig {
    unsigned int encoder_a  = 27;   // EC11 A 相 → GPIO27 (现场实测方向)
    unsigned int encoder_b  = 17;   // EC11 B 相 → GPIO17 (现场实测方向)
    unsigned int encoder_sw = 22;   // EC11 SW → GPIO22

    /** 手工指定 chip 路径, 空字符串 = 自动探测 */
    std::string chip_path;

    /**
     * 平台 profile (V6):
     *   "auto" (空) = 自动探测
     *   "pi5"       = Raspberry Pi 5 显式 profile (按 preferred_label 解析)
     */
    std::string profile;

    /**
     * R7 (B1): 期望的 controller 标签 (case-insensitive 匹配 name 或 label)。
     * 空 = 回退到旧打分策略。Pi 5 用 "pinctrl-rp1"。
     */
    std::string preferred_label;

    /**
     * Raspberry Pi 5 现场验证 profile:
     *   A=27 / B=17 / SW=22; controller 按 pinctrl-rp1 唯一解析
     *   (不再固定 /dev/gpiochip4 数字索引)。
     */
    static GpioConfig forPi5() {
        GpioConfig c;
        c.encoder_a  = 27;
        c.encoder_b  = 17;
        c.encoder_sw = 22;
        c.profile    = "pi5";
        c.preferred_label = "pinctrl-rp1";
        return c;
    }

    /**
     * 验证配置有效性
     * @throws std::invalid_argument 如果配置无效
     */
    void validate() const {
        // 检查重复
        std::set<unsigned int> pins = {encoder_a, encoder_b, encoder_sw};
        if (pins.size() != 3) {
            throw std::invalid_argument(
                "GpioConfig: duplicate pin assignments "
                "(A=" + std::to_string(encoder_a) +
                " B=" + std::to_string(encoder_b) +
                " SW=" + std::to_string(encoder_sw) + ")");
        }

        // 检查合理范围 (BCM GPIO 通常 < 64, 但预留到 255)
        for (auto pin : {encoder_a, encoder_b, encoder_sw}) {
            if (pin > 255) {
                throw std::invalid_argument(
                    "GpioConfig: pin " + std::to_string(pin) + " out of range");
            }
        }
        // R7 (B1): 不再要求 chip_path 等于 /dev/gpiochip4 —
        // 路径合法性由运行时解析与存在性检查负责。
    }

    /** 返回是否有效 (不抛异常版本) */
    bool valid() const noexcept {
        std::set<unsigned int> pins = {encoder_a, encoder_b, encoder_sw};
        if (pins.size() != 3) return false;
        for (auto pin : {encoder_a, encoder_b, encoder_sw}) {
            if (pin > 255) return false;
        }
        return true;
    }
};

/** 枚举出的 gpiochip 候选 (纯数据, 便于单测 fixture)。 */
struct GpioChipCandidate {
    std::string path;     // 如 /dev/gpiochip0
    std::string name;
    std::string label;
    unsigned int num_lines = 0;
};

namespace detail {
inline std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace detail

/**
 * R7 (B1): 纯解析逻辑 (三入口与单测共享, 不依赖 cwd/设备文件)。
 *
 * 优先级:
 *  1. env_override (HOLOPET_GPIO_CHIP) / cfg.chip_path 显式覆盖 —
 *     必须精确命中一个已枚举候选, 否则 fail closed (不回退)。
 *  2. preferred_label 唯一匹配 (case-insensitive 匹配 name 或 label):
 *     0 个 → 抛错; >1 个 → 抛歧义错; 恰 1 个 → 选择。
 *  3. preferred_label 为空 → 旧打分策略 (rp1/pinctrl/bcm/gpio 加权,
 *     并列最高分 fail closed)。
 *
 * @throws std::runtime_error 0 候选 / 多候选歧义 / 覆盖未命中
 */
inline std::string resolveGpioChipPath(
    const GpioConfig& cfg,
    const std::vector<GpioChipCandidate>& candidates,
    const std::string& env_override = "") {

    const std::string& override_p =
        env_override.empty() ? cfg.chip_path : env_override;

    if (!override_p.empty()) {
        for (const auto& c : candidates) {
            if (c.path == override_p) return c.path;
        }
        throw std::runtime_error(
            "GpioChip: override chip not found among enumerated gpiochips: " +
            override_p + " (fail closed, 不回退自动探测)");
    }

    // 标签唯一匹配 (Pi 5: pinctrl-rp1, 数字索引无关)
    if (!cfg.preferred_label.empty()) {
        std::vector<std::string> hits;
        for (const auto& c : candidates) {
            auto want = detail::lowerAscii(cfg.preferred_label);
            if (detail::lowerAscii(c.label) == want ||
                detail::lowerAscii(c.name) == want) {
                hits.push_back(c.path);
            }
        }
        if (hits.empty()) {
            throw std::runtime_error(
                "GpioChip: 未找到 label/name=" + cfg.preferred_label +
                " 的 gpiochip (0 候选, fail closed)");
        }
        if (hits.size() > 1) {
            std::string msg = "GpioChip: label/name=" + cfg.preferred_label +
                              " 匹配多个 gpiochip (歧义, fail closed):";
            for (const auto& p : hits) msg += " " + p;
            throw std::runtime_error(msg);
        }
        return hits[0];
    }

    // 旧打分策略 (无 preferred_label 时)
    std::vector<std::pair<int, std::string>> scored;
    for (const auto& c : candidates) {
        int score = 0;
        auto name  = detail::lowerAscii(c.name);
        auto label = detail::lowerAscii(c.label);
        if (label.find("rp1") != std::string::npos ||
            name.find("rp1") != std::string::npos) score += 100;
        if (label.find("pinctrl") != std::string::npos ||
            label.find("bcm") != std::string::npos ||
            name.find("bcm") != std::string::npos) score += 80;
        if (label.find("gpio") != std::string::npos) score += 50;
        score += std::min(static_cast<int>(c.num_lines / 8), 20);
        scored.push_back({score, c.path});
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (scored.empty()) {
        throw std::runtime_error("GpioChip: no gpiochip candidates (fail closed)");
    }
    if (scored.size() > 1 && scored[0].first == scored[1].first) {
        std::string msg = "GpioChip: multiple gpiochip candidates with same "
                          "highest score (fail closed):";
        for (const auto& s : scored) {
            if (s.first < scored[0].first) break;
            msg += " " + s.second;
        }
        throw std::runtime_error(msg);
    }
    return scored[0].second;
}

} // namespace holopet
