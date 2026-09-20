/**
 * test_gpio_config.cpp — GpioConfig 纯逻辑测试  V2-R2
 *
 * R2 修复: 字符串拼接 → std::string, 消除 missing-field warnings
 *
 * 测试: 默认值 / 无重复 / 有效范围 / validate 异常 / valid 判据
 */

#include "hardware/gpio_config.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <set>
#include <vector>

using namespace holopet;

static int g_passed = 0;
static int g_failed = 0;

void check(bool cond, const std::string& test_name) {
    if (cond) {
        std::cout << "  PASS: " << test_name << '\n';
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << test_name << '\n';
        ++g_failed;
    }
}

// R7 (B1/B4): 纯解析器 fixture
static GpioChipCandidate chip(const std::string& path, const std::string& label,
                              unsigned int lines = 54) {
    GpioChipCandidate c;
    c.path = path;
    c.name = label;
    c.label = label;
    c.num_lines = lines;
    return c;
}

static bool resolves_to(const GpioConfig& cfg,
                        const std::vector<GpioChipCandidate>& cands,
                        const std::string& expect,
                        const std::string& env_override = "") {
    try {
        return resolveGpioChipPath(cfg, cands, env_override) == expect;
    } catch (...) {
        return false;
    }
}

static bool throws_for(const GpioConfig& cfg,
                       const std::vector<GpioChipCandidate>& cands,
                       const std::string& env_override = "") {
    try {
        resolveGpioChipPath(cfg, cands, env_override);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

int main() {
    std::cout << "=== test_gpio_config (V2-R2 + R7 resolver) ===\n\n";

    GpioConfig cfg;

    // ---- 默认值 (V6 现场差异合入: Pi 5 实测旋钮方向 A27/B17/SW22) ----
    std::cout << "[默认值]\n";
    check(cfg.encoder_a  == 27, "encoder_a = 27 (现场锁定)");
    check(cfg.encoder_b  == 17, "encoder_b = 17 (现场锁定)");
    check(cfg.encoder_sw == 22, "encoder_sw = 22");
    check(cfg.chip_path.empty(), "chip_path 默认为空");
    check(cfg.profile.empty(), "profile 默认 auto");

    // ---- 无重复 ----
    std::cout << "\n[无重复]\n";
    {
        std::set<unsigned int> pins = {cfg.encoder_a, cfg.encoder_b, cfg.encoder_sw};
        check(pins.size() == 3, "A/B/SW 各不相同");
    }

    // ---- 有效范围 ----
    std::cout << "\n[有效范围]\n";
    check(cfg.encoder_a  < 256, "encoder_a < 256");
    check(cfg.encoder_b  < 256, "encoder_b < 256");
    check(cfg.encoder_sw < 256, "encoder_sw < 256");

    // ---- valid() ----
    std::cout << "\n[valid()]\n";
    check(cfg.valid(), "默认配置 valid() = true");

    {
        GpioConfig dup{17, 17, 22, ""};
        check(!dup.valid(), "重复 pin (17,17,22) valid() = false");
    }
    {
        GpioConfig out{300, 27, 22, ""};
        check(!out.valid(), "超范围 pin (300) valid() = false");
    }

    // ---- validate() 异常 ----
    std::cout << "\n[validate() 异常]\n";
    try {
        cfg.validate();
        check(true, "默认 validate() 不抛异常");
    } catch (const std::exception& ex) {
        check(false, std::string("默认 validate() 不抛异常, 但抛了: ") + ex.what());
    }

    try {
        GpioConfig dup{17, 17, 22, ""};
        dup.validate();
        check(false, "重复 pin validate() 应抛异常");
    } catch (const std::invalid_argument&) {
        check(true, "重复 pin → std::invalid_argument");
    }

    try {
        GpioConfig out{300, 27, 22, ""};
        out.validate();
        check(false, "超范围 pin validate() 应抛异常");
    } catch (const std::invalid_argument&) {
        check(true, "超范围 pin → std::invalid_argument");
    }

    // ---- 可自定义 ----
    std::cout << "\n[可自定义]\n";
    {
        GpioConfig custom{5, 6, 13, "/dev/gpiochip4"};
        check(custom.encoder_a  == 5,  "自定义 a=5");
        check(custom.encoder_b  == 6,  "自定义 b=6");
        check(custom.encoder_sw == 13, "自定义 sw=13");
        check(custom.chip_path   == "/dev/gpiochip4", "自定义 chip_path");
        check(custom.valid(), "自定义有效配置 valid() = true");
    }

    // ---- R7 (B1/B4): pinctrl-rp1 动态解析 ----
    std::cout << "\n[R7 pinctrl-rp1 动态解析]\n";
    {
        GpioConfig pi5 = GpioConfig::forPi5();
        check(pi5.encoder_a == 27 && pi5.encoder_b == 17 && pi5.encoder_sw == 22,
              "forPi5 线号保持 27/17/22");
        check(pi5.chip_path.empty(), "forPi5 不再硬编码 chip_path");
        check(pi5.preferred_label == "pinctrl-rp1",
              "forPi5 preferred_label=pinctrl-rp1");

        // 数字索引无关: rp1 在 chip0 → chip0; 在 chip4 → chip4
        check(resolves_to(pi5, {chip("/dev/gpiochip0", "pinctrl-rp1")},
                          "/dev/gpiochip0"),
              "rp1@gpiochip0 → 选 chip0");
        check(resolves_to(pi5, {chip("/dev/gpiochip4", "pinctrl-rp1")},
                          "/dev/gpiochip4"),
              "rp1@gpiochip4 → 选 chip4 (数字无关)");
        check(resolves_to(pi5,
                          {chip("/dev/gpiochip0", "dummy1"),
                           chip("/dev/gpiochip4", "pinctrl-rp1")},
                          "/dev/gpiochip4"),
              "多候选时精确挑出 rp1");

        // 0 候选 / 歧义 → fail closed
        check(throws_for(pi5, {chip("/dev/gpiochip0", "other")}),
              "无 pinctrl-rp1 → throw");
        check(throws_for(pi5,
                         {chip("/dev/gpiochip0", "pinctrl-rp1"),
                          chip("/dev/gpiochip4", "pinctrl-rp1")}),
              "两个同名候选 → throw (歧义)");

        // 显式覆盖: 合法命中; 未命中 fail closed 不回退
        check(resolves_to(pi5, {chip("/dev/gpiochip2", "pinctrl-rp1")},
                          "/dev/gpiochip2", "/dev/gpiochip2"),
              "HOLOPET_GPIO_CHIP 合法覆盖 → 优先");
        check(throws_for(pi5, {chip("/dev/gpiochip0", "pinctrl-rp1")},
                         "/dev/gpiochip9"),
              "覆盖路径不存在 → throw (不回退)");
        check(throws_for(pi5, {chip("/dev/gpiochip0", "pinctrl-rp1")},
                         "/dev/null"),
              "覆盖非 chip → throw (不回退)");

        // 无 preferred_label → 旧打分策略仍可用 (非 Pi 平台)
        GpioConfig legacy;
        legacy.preferred_label = "";
        check(resolves_to(legacy, {chip("/dev/gpiochip0", "pinctrl-bcm2835")},
                          "/dev/gpiochip0"),
              "无 label 时旧打分策略可用");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
