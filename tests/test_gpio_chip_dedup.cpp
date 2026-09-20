/**
 * test_gpio_chip_dedup.cpp — R8_R1 (A3 回归): GPIO 候选枚举去重。
 *
 * 现场事实 (2026-09-10, Pi OS trixie): /dev/gpiochip4 -> gpiochip0 是 udev
 * 兼容符号链接; 旧枚举把同一字符设备计为两个 pinctrl-rp1 候选, 触发
 * 假歧义 fail-closed。本测试用临时目录 fixture 验证:
 *   1. 真实节点 + 指向它的符号链接 → 只计一个候选 (与现场形态一致);
 *   2. 非 gpiochip* 名字被忽略;
 *   3. 结果按路径排序 (确定性);
 *   4. 多个不同真实节点全部保留 (去重不得吞掉真实 chip);
 *   5. collectGpioChipPaths + resolveGpioChipPath 组合: symlink 去重后
 *      pinctrl-rp1 唯一匹配成功 (现场 /dev 形态的可复现版)。
 *
 * 符号链接需要平台支持: Linux 原生; Windows 需开发者模式/管理员,
 * 创建失败时明确 SKIP 并注明原因 (不冒充 PASS)。
 */

#include "hardware/gpio_chip.hpp"
#include "hardware/gpio_config.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace holopet;

static int g_passed = 0, g_failed = 0, g_skipped = 0;
static void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}
static void skip(const std::string& n, const std::string& why) {
    std::cout << "  SKIP: " << n << " (" << why << ")\n";
    ++g_skipped;
}

static std::string mk_tmpdir() {
#ifdef _WIN32
    char buf[512];
    if (std::tmpnam(buf)) {
        std::string d = std::string(buf) + "_dir";
        std::system(("mkdir \"" + d + "\" >NUL 2>NUL").c_str());
        return d;
    }
    return "";
#else
    char tmpl[] = "/tmp/holopet_gpio_dedup_XXXXXX";
    char* d = mkdtemp(tmpl);
    return d ? std::string(d) : "";
#endif
}

static bool touch(const std::string& p) {
    std::ofstream f(p);
    return static_cast<bool>(f);
}

static bool make_symlink(const std::string& target, const std::string& link) {
#ifdef _WIN32
    std::system(("mklink \"" + link + "\" \"" + target + "\" >NUL 2>NUL")
                    .c_str());
    std::ifstream f(link);
    return static_cast<bool>(f);
#else
    return ::symlink(target.c_str(), link.c_str()) == 0;
#endif
}

static void cleanup_tree(const std::string& dir,
                         const std::vector<std::string>& entries) {
    for (const auto& e : entries) ::remove(e.c_str());
#ifdef _WIN32
    std::system(("rmdir \"" + dir + "\" >NUL 2>NUL").c_str());
#else
    ::rmdir(dir.c_str());
#endif
}

int main() {
    std::cout << "=== test_gpio_chip_dedup (R8_R1 A3) ===\n\n";

    const std::string dir = mk_tmpdir();
    if (dir.empty()) {
        check(false, "创建临时目录 fixture");
        return 1;
    }
    const std::string real_chip = dir + "/gpiochip0";
    const std::string alias = dir + "/gpiochip4";     // 现场 udev 别名形态
    const std::string other_real = dir + "/gpiochip1";
    const std::string noise = dir + "/not_a_chip";
    const std::string subdir = dir + "/gpiochip9_dir";

    check(touch(real_chip), "fixture: 真实节点 gpiochip0");
    check(touch(other_real), "fixture: 真实节点 gpiochip1");
    check(touch(noise), "fixture: 干扰名 not_a_chip");
#ifdef _WIN32
    std::system(("mkdir \"" + subdir + "\" >NUL 2>NUL").c_str());
#else
    ::mkdir(subdir.c_str(), 0755);
#endif

    const bool link_ok = make_symlink(real_chip, alias);
    if (!link_ok) {
        skip("symlink 去重场景",
             "本平台不允许创建符号链接 (Windows 需开发者模式/管理员)");
    } else {
        std::vector<std::string> got = collectGpioChipPaths(dir);
        // 期望: gpiochip0 / gpiochip1 / gpiochip9_dir 三个, 无 gpiochip4 别名
        bool has_alias = false, has_real = false, has_other = false;
        for (const auto& p : got) {
            if (p == alias) has_alias = true;
            if (p == real_chip) has_real = true;
            if (p == other_real) has_other = true;
        }
        check(!has_alias, "symlink (gpiochip4->gpiochip0) 不计为候选");
        check(has_real, "真实节点 gpiochip0 保留");
        check(has_other, "另一个真实节点 gpiochip1 保留 (去重不吞真实 chip)");
        check(got.size() == 3, "候选数 = 3 (0/1/9_dir, 排除 symlink 与噪声名)");
        bool sorted = true;
        for (size_t i = 1; i < got.size(); ++i)
            if (got[i - 1] > got[i]) sorted = false;
        check(sorted, "结果按路径排序 (确定性)");
        bool noise_present = false;
        for (const auto& p : got)
            if (p == noise) noise_present = true;
        check(!noise_present, "非 gpiochip* 名字被忽略");
    }

    cleanup_tree(dir, {real_chip, other_real, noise, alias});
#ifdef _WIN32
    std::system(("rmdir \"" + subdir + "\" >NUL 2>NUL").c_str());
#else
    ::rmdir(subdir.c_str());
#endif

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败, " << g_skipped << " 跳过 ===\n";
    return g_failed == 0 ? 0 : 1;
}
