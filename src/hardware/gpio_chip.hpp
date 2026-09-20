#pragma once
/**
 * gpio_chip.hpp — GPIO Chip RAII 封装 (libgpiod 2.x C++ binding)  V2-R1
 *
 * R1 修复:
 *  - 不再用 /dev/null 占位: 使用 std::optional<gpiod::chip>
 *  - auto-detect 增强: label/name 优先匹配, 支持 chip_path 手工 override
 *  - read() 验证 offset 属于本次 request
 *
 * 功能:
 *  - 自动枚举 /dev/gpiochip* 并选择包含目标 pin 的 chip
 *  - RAII 管理 chip 和 line_request 生命周期
 *  - Pull-Up 输入配置
 *  - 禁止 copy，允许 move
 *
 * 依赖: libgpiod 2.x (<gpiod.hpp>)
 */

#include "gpio_config.hpp"
#include "system/logger.hpp"

#include <gpiod.hpp>

#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <set>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <algorithm>
#include <cctype>

namespace holopet {

/**
 * R8_R1 (A3 回归): gpiochip* 候选路径枚举 (纯文件系统逻辑, 可单测)。
 * 规则:
 *   - 只接受文件名以 "gpiochip" 开头;
 *   - 跳过符号链接 — Pi OS 上 /dev/gpiochip4 -> gpiochip0 是 udev 兼容
 *     别名, 同一字符设备不得计为两个候选 (R8 现场歧义根因);
 *   - 不在此层校验设备可打开性 (由上层 gpiod::chip 探测), 非法节点
 *     打开失败即跳过;
 *   - 按路径排序, 结果确定。
 */
inline std::vector<std::string> collectGpioChipPaths(
    const std::string& dir) {
    std::vector<std::string> paths;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto fname = entry.path().filename().string();
        if (fname.rfind("gpiochip", 0) != 0) continue;
        if (entry.is_symlink()) continue;
        paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

class GpioChip {
public:
    // R3-R1 (缺陷 A 同型): 默认实参拆分, GCC/Clang 兼容; 行为不变
    GpioChip() : GpioChip(GpioConfig()) {}
    explicit GpioChip(const GpioConfig& cfg)
        : cfg_(cfg)
    {
        cfg_.validate();

        // 收集需要访问的 pin
        requested_offsets_ = {cfg_.encoder_a, cfg_.encoder_b, cfg_.encoder_sw};

        // ---- Step 1: 确定 chip path ----
        std::string path = detectChipPath();

        // ---- Step 2: 构造 chip ----
        chip_.emplace(path);
        auto info = chip_->get_info();
        LOG_INFO("GPIO") << "selected " << path
                         << " name=" << info.name()
                         << " label=" << info.label()
                         << " lines=" << info.num_lines();

        // ---- Step 3: 验证所有 line offset 可用 ----
        for (auto off : requested_offsets_) {
            try {
                auto li = chip_->get_line_info(off);
                LOG_INFO("GPIO") << "  line " << off
                                 << " name=" << li.name()
                                 << " consumer=" << li.consumer();
            } catch (const std::exception& ex) {
                throw std::runtime_error(
                    "GpioChip: line " + std::to_string(off) +
                    " unavailable on " + path + ": " + ex.what());
            }
        }

        // ---- Step 4: 配置并 request lines ----
        gpiod::line_settings settings;
        settings.set_direction(gpiod::line::direction::INPUT)
                .set_bias(gpiod::line::bias::PULL_UP);

        gpiod::line::offsets offsets(requested_offsets_.begin(),
                                      requested_offsets_.end());

        auto builder = chip_->prepare_request();
        builder.set_consumer("holopet")
               .add_line_settings(offsets, settings);

        request_.emplace(builder.do_request());
    }

    ~GpioChip() = default;

    // 禁止 copy
    GpioChip(const GpioChip&) = delete;
    GpioChip& operator=(const GpioChip&) = delete;

    // 禁止 move (moved-from optional<gpiod::chip> 不安全)
    GpioChip(GpioChip&&) = delete;
    GpioChip& operator=(GpioChip&&) = delete;

    /**
     * 读取指定 offset 的 GPIO 电平 (V6: 非 const — 现场 libgpiod 2.x 实测
     * 需要非 const line_request 调用路径, 并允许实现内缓存/重试)
     * @param offset  必须是 encoder_a / encoder_b / encoder_sw 之一
     * @return true = HIGH, false = LOW
     * @throws std::invalid_argument 如果 offset 不在已 request 的列表中
     */
    bool read(unsigned int offset) {
        if (requested_offsets_.count(static_cast<unsigned int>(offset)) == 0) {
            throw std::invalid_argument(
                "GpioChip::read: offset " + std::to_string(offset) +
                " not in requested set");
        }
        auto val = request_->get_value(offset);
        return (val == gpiod::line::value::ACTIVE);
    }

    /** 显式释放资源 (析构自动处理, 也可手动提前释放) */
    void close() noexcept {
        // 先释放 line_request (依赖 chip), 再释放 chip
        request_.reset();
        chip_.reset();
    }

    explicit operator bool() const { return chip_.has_value(); }

private:
    GpioConfig cfg_;
    std::optional<gpiod::chip> chip_;
    std::optional<gpiod::line_request> request_;
    std::set<unsigned int> requested_offsets_;

    /**
     * 探测 chip path (R7 B1: 枚举候选 → 共享纯解析 resolveGpioChipPath)
     * 1. HOLOPET_GPIO_CHIP 环境变量 / cfg.chip_path 显式覆盖 —
     *    必须命中一个已枚举候选 (存在且可打开), 否则 fail closed。
     * 2. cfg.preferred_label 唯一匹配 (Pi 5: pinctrl-rp1, 数字无关)。
     * 3. 旧打分策略 (无 preferred_label 时), 并列最高分 fail closed。
     */
    std::string detectChipPath() {
        std::vector<GpioChipCandidate> candidates;
        unsigned int max_offset = *requested_offsets_.rbegin();

        for (const auto& path : collectGpioChipPaths("/dev")) {
            try {
                gpiod::chip probe(path);
                auto info = probe.get_info();

                GpioChipCandidate cand;
                cand.path      = path;
                cand.name      = info.name();
                cand.label     = info.label();
                cand.num_lines = info.num_lines();

                LOG_INFO("GPIO") << "candidate " << cand.path
                                 << " name=" << cand.name
                                 << " label=" << cand.label
                                 << " lines=" << cand.num_lines;

                // 基础条件: 有足够的 lines
                if (cand.num_lines <= max_offset) {
                    continue;
                }

                // 验证关键 line 是否存在
                bool all_ok = true;
                for (auto off : requested_offsets_) {
                    try {
                        auto li = probe.get_line_info(off);
                        (void)li;
                    } catch (...) {
                        all_ok = false;
                        break;
                    }
                }
                if (!all_ok) {
                    continue;
                }

                candidates.push_back(std::move(cand));

            } catch (const std::exception& ex) {
                LOG_WARN("GPIO") << "chip " << path
                                 << " skip: " << ex.what();
            }
        }

        // R7 (B1): 环境变量覆盖 (HOLOPET_GPIO_CHIP), 供诊断
        const char* env_chip = std::getenv("HOLOPET_GPIO_CHIP");
        std::string chosen = resolveGpioChipPath(
            cfg_, candidates, env_chip ? env_chip : std::string());

        // 记录最终选择与线号 (B2.6)
        for (const auto& c : candidates) {
            if (c.path == chosen) {
                LOG_INFO("GPIO") << "selected GPIO controller: " << chosen
                                 << " (label=" << c.label
                                 << ", name=" << c.name
                                 << ", lines=" << c.num_lines << ")"
                                 << " pins A=" << cfg_.encoder_a
                                 << " B=" << cfg_.encoder_b
                                 << " SW=" << cfg_.encoder_sw;
                break;
            }
        }
        return chosen;
    }
};

} // namespace holopet
