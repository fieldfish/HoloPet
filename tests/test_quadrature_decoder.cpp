/**
 * test_quadrature_decoder.cpp — QuadratureDecoder 测试
 *
 * 测试: 正转 / 反转 / 非法跳变 / 初始化 / 多圈旋转
 */

#include "input/quadrature_decoder.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace holopet;

static int g_passed = 0;
static int g_failed = 0;

void check(bool cond, const char* test_name) {
    if (cond) {
        std::cout << "  PASS: " << test_name << '\n';
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << test_name << '\n';
        ++g_failed;
    }
}

// 辅助: 喂一系列 (A,B) 状态, 收集产生的所有事件
std::vector<EncoderEvent> feedSequence(QuadratureDecoder& dec,
                                        const std::vector<std::pair<bool,bool>>& states) {
    std::vector<EncoderEvent> events;
    std::uint64_t t = 0;
    for (auto [a, b] : states) {
        auto ev = dec.update(a, b, t);
        if (ev.has_value()) {
            events.push_back(ev.value());
        }
        t += 100;  // 模拟时间推进
    }
    return events;
}

int main() {
    std::cout << "=== test_quadrature_decoder ===\n\n";

    // ---- 1. 初始化测试 ----
    std::cout << "[初始化测试]\n";
    {
        QuadratureDecoder dec;
        // 首次调用: 任意状态, 不产生事件
        auto ev1 = dec.update(false, false, 0);  // 00
        check(!ev1.has_value(), "首次 update(0,0) 不产生事件");

        auto ev2 = dec.update(true, false, 100);  // 10
        check(!ev2.has_value(), "首次 update(1,0) 也不产生事件 (只记录)");
    }

    // ---- 2. 正转测试: 完整 CW 序列 ----
    // 00→01→11→10→00 应产生 1 次 Clockwise
    std::cout << "\n[正转测试]\n";
    {
        QuadratureDecoder dec;

        // 先初始化
        dec.update(false, false, 0);

        // 完整正转序列
        std::vector<std::pair<bool,bool>> cw_seq = {
            {false, false},  // 00 (起始)
            {false, true},   // 01
            {true,  true},   // 11
            {true,  false},  // 10
            {false, false},  // 00
        };
        auto events = feedSequence(dec, cw_seq);
        check(events.size() == 1, "完整 CW 序列产生 1 个事件");
        if (!events.empty()) {
            check(events[0] == EncoderEvent::Clockwise, "事件 = Clockwise");
        }
    }

    // ---- 3. 反转测试: 完整 CCW 序列 ----
    // 00→10→11→01→00 应产生 1 次 CounterClockwise
    std::cout << "\n[反转测试]\n";
    {
        QuadratureDecoder dec;
        dec.update(false, false, 0);

        std::vector<std::pair<bool,bool>> ccw_seq = {
            {false, false},  // 00
            {true,  false},  // 10
            {true,  true},   // 11
            {false, true},   // 01
            {false, false},  // 00
        };
        auto events = feedSequence(dec, ccw_seq);
        check(events.size() == 1, "完整 CCW 序列产生 1 个事件");
        if (!events.empty()) {
            check(events[0] == EncoderEvent::CounterClockwise, "事件 = CounterClockwise");
        }
    }

    // ---- 4. 非法跳变测试 ----
    std::cout << "\n[非法跳变测试]\n";
    {
        QuadratureDecoder dec;
        dec.update(false, false, 0);  // init at 00

        // 00→11 非法跳变
        auto ev = dec.update(true, true, 100);
        check(!ev.has_value(), "00→11 非法跳变不产生事件");

        // 非法跳变后累加器清零, 从 11 开始重新计数
        // 继续走 11→10→00 只走了 2 步, 不到 4 步不产生事件
        auto ev2 = dec.update(true, false, 200);   // 11→10
        check(!ev2.has_value(), "非法后 11→10 不产生事件 (累加器已清零)");
    }

    // 非法跳变后完整走一圈应该还能产生事件
    std::cout << "\n[非法跳变后恢复测试]\n";
    {
        QuadratureDecoder dec;
        dec.update(false, false, 0);   // 00
        dec.update(true, true, 100);   // 00→11 非法

        // 重置后从 11 开始走 CW
        auto ev = dec.update(true, false, 200);   // 11→10 (+1)
        check(!ev.has_value(), "11→10 (+1, 累加器=1)");

        ev = dec.update(false, false, 300);        // 10→00 (+1, 累加器=2)
        check(!ev.has_value(), "10→00 (+1, 累加器=2)");

        ev = dec.update(false, true, 400);         // 00→01 (+1, 累加器=3)
        check(!ev.has_value(), "00→01 (+1, 累加器=3)");

        ev = dec.update(true, true, 500);          // 01→11 (+1, 累加器=4 → CW)
        check(ev.has_value() && ev.value() == EncoderEvent::Clockwise,
              "01→11 满 4 步 → Clockwise");
    }

    // ---- 5. 多圈旋转测试 ----
    std::cout << "\n[多圈旋转测试]\n";
    {
        QuadratureDecoder dec;
        dec.update(false, false, 0);

        // 连续正转 3 圈
        int cw_count = 0;
        int ccw_count = 0;
        std::uint64_t t = 100;

        // 3 圈 CW (每圈 4 步)
        for (int lap = 0; lap < 3; ++lap) {
            // 00→01→11→10→00
            bool steps[5][2] = {{0,0},{0,1},{1,1},{1,0},{0,0}};
            for (int i = 0; i < 5; ++i) {
                auto ev = dec.update(steps[i][0], steps[i][1], t);
                if (ev.has_value()) {
                    if (ev.value() == EncoderEvent::Clockwise) ++cw_count;
                    else ++ccw_count;
                }
                t += 100;
            }
        }
        check(cw_count == 3, "3圈正转 → 3次 Clockwise");
        check(ccw_count == 0, "0次 CounterClockwise");

        // 再反转 2 圈
        for (int lap = 0; lap < 2; ++lap) {
            bool steps[5][2] = {{0,0},{1,0},{1,1},{0,1},{0,0}};
            for (int i = 0; i < 5; ++i) {
                auto ev = dec.update(steps[i][0], steps[i][1], t);
                if (ev.has_value()) {
                    if (ev.value() == EncoderEvent::Clockwise) ++cw_count;
                    else ++ccw_count;
                }
                t += 100;
            }
        }
        check(cw_count == 3, "反转后 CW 仍 = 3");
        check(ccw_count == 2, "2圈反转 → 2次 CounterClockwise");
    }

    // ---- 6. 单步来回测试 (中途反向) ----
    std::cout << "\n[中途反向测试]\n";
    {
        QuadratureDecoder dec;
        dec.update(false, false, 0);   // 00

        dec.update(false, true, 100);  // 00→01 (+1)
        dec.update(true, true, 200);   // 01→11 (+1, acc=2)
        // 中途反向
        dec.update(false, true, 300);  // 11→01 (-1, acc=1)
        dec.update(false, false, 400); // 01→00 (-1, acc=0)
        // 回到起点, 不产生事件
        auto ev = dec.update(false, false, 500);
        check(!ev.has_value(), "中途反向回到00, 不产生事件");
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
