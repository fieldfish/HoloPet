/**
 * test_pi5_profile.cpp — Pi 5 现场 profile 锁定测试  V6 → R7
 *
 * R7 (B1, 经用户批准加入允许修改清单): 不再锁定 /dev/gpiochip4 数字编号 —
 *   forPi5() 默认 chip_path 为空, 以 preferred_label="pinctrl-rp1" 动态解析;
 *   validate() 不得因 gpiochip0/gpiochip4 的数字编号本身拒绝显式路径
 *   (路径真实性由运行时解析/硬件检查负责)。
 * 保持不变: A=27 / B=17 / SW=22 线号契约。
 */

#include "hardware/gpio_config.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace holopet;

static int g_passed = 0;
static int g_failed = 0;

void check(bool cond, const std::string& test_name) {
    if (cond) { std::cout << "  PASS: " << test_name << '\n'; ++g_passed; }
    else      { std::cout << "  FAIL: " << test_name << '\n'; ++g_failed; }
}

static GpioChipCandidate chip(const std::string& path, const std::string& label,
                              unsigned int lines = 54) {
    GpioChipCandidate c;
    c.path = path;
    c.name = label;
    c.label = label;
    c.num_lines = lines;
    return c;
}

int main() {
    std::cout << "=== test_pi5_profile (V6 → R7 契约) ===\n\n";

    GpioConfig cfg = GpioConfig::forPi5();

    std::cout << "[Pi5 profile 默认]\n";
    check(cfg.encoder_a  == 27, "A = GPIO27");
    check(cfg.encoder_b  == 17, "B = GPIO17");
    check(cfg.encoder_sw == 22, "SW = GPIO22");
    check(cfg.chip_path.empty(), "chip_path 默认为空 (不硬编码数字编号)");
    check(cfg.preferred_label == "pinctrl-rp1", "preferred_label = pinctrl-rp1");
    check(cfg.profile    == "pi5", "profile = pi5");
    check(cfg.valid(), "valid() = true");

    std::cout << "\n[validate()]\n";
    try {
        cfg.validate();
        check(true, "pi5 profile validate() 不抛异常");
    } catch (const std::exception& ex) {
        check(false, std::string("pi5 validate() 意外抛异常: ") + ex.what());
    }

    // R7: 显式指定 gpiochip0/gpiochip4 都不因数字编号本身被拒绝
    {
        GpioConfig explicit0 = GpioConfig::forPi5();
        explicit0.chip_path = "/dev/gpiochip0";
        try {
            explicit0.validate();
            check(true, "pi5 + 显式 /dev/gpiochip0 validate() 不抛异常");
        } catch (const std::invalid_argument&) {
            check(false, "pi5 + /dev/gpiochip0 不应因数字编号被拒绝");
        }
    }
    {
        GpioConfig explicit4 = GpioConfig::forPi5();
        explicit4.chip_path = "/dev/gpiochip4";
        try {
            explicit4.validate();
            check(true, "pi5 + 显式 /dev/gpiochip4 validate() 不抛异常");
        } catch (const std::invalid_argument&) {
            check(false, "pi5 + /dev/gpiochip4 不应因数字编号被拒绝");
        }
    }

    std::cout << "\n[解析器: 数字编号无关]\n";
    {
        std::vector<GpioChipCandidate> c0 = {chip("/dev/gpiochip0", "pinctrl-rp1")};
        std::vector<GpioChipCandidate> c4 = {chip("/dev/gpiochip4", "pinctrl-rp1")};
        try {
            check(resolveGpioChipPath(cfg, c0) == "/dev/gpiochip0",
                  "rp1@gpiochip0 → 选 chip0");
            check(resolveGpioChipPath(cfg, c4) == "/dev/gpiochip4",
                  "rp1@gpiochip4 → 选 chip4 (数字无关)");
        } catch (const std::exception& ex) {
            check(false, std::string("解析意外抛异常: ") + ex.what());
        }
    }

    std::cout << "\n[解析器: 失败场景 fail closed]\n";
    {
        std::vector<GpioChipCandidate> none = {chip("/dev/gpiochip0", "other")};
        std::vector<GpioChipCandidate> dup = {
            chip("/dev/gpiochip0", "pinctrl-rp1"),
            chip("/dev/gpiochip4", "pinctrl-rp1")};
        try {
            resolveGpioChipPath(cfg, none);
            check(false, "无 pinctrl-rp1 应抛异常");
        } catch (const std::runtime_error&) {
            check(true, "无 pinctrl-rp1 → 抛异常 (fail closed)");
        }
        try {
            resolveGpioChipPath(cfg, dup);
            check(false, "多匹配应抛异常");
        } catch (const std::runtime_error&) {
            check(true, "多匹配 → 抛异常 (fail closed)");
        }
    }

    std::cout << "\n[与默认配置差异]\n";
    GpioConfig plain;
    check(plain.encoder_a  == cfg.encoder_a,  "默认 A 已同步现场值");
    check(plain.chip_path.empty(),             "默认 chip_path 仍自动探测");
    check(plain.profile.empty(),               "默认 profile = auto");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
