/**
 * test_exit_guard_move_only.cpp — R4-R1 P4 (D8): ExitGuard move-only
 *
 * 断言:
 *  - 复制构造/复制赋值被删除 (编译期: 用 static_assert 检测)
 *  - 移动构造/移动赋值可用
 *  - 移动后源对象不再重复收尾 (done 转移)
 */

#include "ipc/exit_guard.hpp"

#include <iostream>
#include <type_traits>

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_exit_guard_move_only (V6 R4-R1) ===\n\n";

    // 编译期: 复制被删除 / 移动可用
    check(!std::is_copy_constructible<ExitGuard>::value, "复制构造已删除");
    check(!std::is_copy_assignable<ExitGuard>::value, "复制赋值已删除");
    check(std::is_move_constructible<ExitGuard>::value, "移动构造可用");
    check(std::is_move_assignable<ExitGuard>::value, "移动赋值可用");

    // 移动后源对象 done=true (不重复收尾), 目标接管收尾
    {
        int count = 0;
        ExitGuard src;
        src.destroy_display = [&count]() { ++count; };
        ExitGuard dst = std::move(src);
        dst.run();
        check(count == 1, "移动后目标执行收尾一次");
        src.run();   // 源已 done — 不重复
        check(count == 1, "移动后源对象不重复收尾");
    }
    {
        // 移动赋值
        int count = 0;
        ExitGuard a;
        ExitGuard b;
        b.destroy_display = [&count]() { ++count; };
        a = std::move(b);
        a.run();
        check(count == 1, "移动赋值后目标执行收尾");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
