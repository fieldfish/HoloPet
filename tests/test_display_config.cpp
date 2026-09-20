/**
 * test_display_config.cpp — DisplayConfig 纯逻辑测试 (V3)
 *
 * 不启动 SDL。
 */

#include "display/display_config.hpp"
#include <iostream>
#include <string>

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

int main() {
    std::cout << "=== test_display_config ===\n\n";

    DisplayConfig cfg;

    // ---- 默认值 ----
    std::cout << "[默认值]\n";
    check(cfg.width == 800, "默认 width = 800");
    check(cfg.height == 800, "默认 height = 800");
    check(cfg.fullscreen == false, "默认 fullscreen = false");
    check(cfg.title == "HoloPet", "默认 title = HoloPet");

    // ---- valid ----
    std::cout << "\n[valid()]\n";
    check(cfg.valid(), "默认配置 valid() = true");

    {
        DisplayConfig bad{0, 800, false, "t"};
        check(!bad.valid(), "width=0 valid() = false");
    }
    {
        DisplayConfig bad{-100, 800, false, "t"};
        check(!bad.valid(), "width<0 valid() = false");
    }
    {
        DisplayConfig bad{800, 0, false, "t"};
        check(!bad.valid(), "height=0 valid() = false");
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
        DisplayConfig bad{0, 800, false, "t"};
        bad.validate();
        check(false, "width=0 validate() 应抛异常");
    } catch (const std::invalid_argument&) {
        check(true, "width=0 → std::invalid_argument");
    }

    try {
        DisplayConfig bad{800, -5, false, "t"};
        bad.validate();
        check(false, "height<0 validate() 应抛异常");
    } catch (const std::invalid_argument&) {
        check(true, "height<0 → std::invalid_argument");
    }

    // ---- 可自定义 ----
    std::cout << "\n[可自定义]\n";
    {
        DisplayConfig custom{1024, 768, true, "MyPet"};
        check(custom.width == 1024, "自定义 width=1024");
        check(custom.height == 768, "自定义 height=768");
        check(custom.fullscreen == true, "自定义 fullscreen=true");
        check(custom.title == "MyPet", "自定义 title");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
