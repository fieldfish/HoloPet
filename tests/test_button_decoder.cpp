/**
 * test_button_decoder.cpp — ButtonDecoder 测试 (V1-R1 返工版)
 *
 * 测试用例 (基于 R1 返工单第 8 节):
 *   [x] 普通短按 → 1 Pressed
 *   [x] 普通长按 → 1 LongPressed
 *   [x] 长按保持不重复
 *   [x] 长按释放不补 Pressed
 *   [x] press bounce 不触发
 *   [x] release bounce 只产生1次
 *   [x] 连续3次短按 → 3 Pressed
 *   [x] reset后可正常短按
 *   [x] 稀疏采样下短按仍正确
 *   [x] 不同轮询间隔不改变长按起点语义
 */

#include "input/button_decoder.hpp"
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

// 喂一系列 (pressed, time_us), 收集所有事件
std::vector<EncoderEvent> feed(ButtonDecoder& dec,
                                const std::vector<std::pair<bool, std::uint64_t>>& steps) {
    std::vector<EncoderEvent> events;
    for (auto [pressed, t] : steps) {
        auto ev = dec.update(pressed, t);
        if (ev.has_value()) events.push_back(ev.value());
    }
    return events;
}

int main() {
    std::cout << "=== test_button_decoder (V1-R1) ===\n\n";

    constexpr std::uint64_t LONG_US = 500'000;   // 长按阈值 500ms
    constexpr std::uint64_t DB_US   = 10'000;     // 消抖 10ms

    // ================================================================
    // 1. 普通短按 → 1 Pressed
    // ================================================================
    std::cout << "[1. 普通短按]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},   // 按下
            {false, 200'000},   // 释放 (100ms 按住, < 500ms)
            {false, 300'000},   // 释放稳定 → 应输出 Pressed
        });
        check(events.size() == 1, "产生 1 个事件");
        check(!events.empty() && events[0] == EncoderEvent::Pressed, "事件 = Pressed");
    }

    // ================================================================
    // 2. 普通长按 → 1 LongPressed
    // ================================================================
    std::cout << "\n[2. 普通长按]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},   // 按下
            {true,  300'000},   // 保持 (press_start ≈ 110ms)
            {true,  700'000},   // now=700 >= 110+500=610 → LongPressed
            {false, 800'000},   // 释放
            {false, 900'000},   // 释放稳定
        });
        check(events.size() == 1, "产生 1 个事件");
        check(!events.empty() && events[0] == EncoderEvent::LongPressed, "事件 = LongPressed");
    }

    // ================================================================
    // 3. 长按保持不重复
    // ================================================================
    std::cout << "\n[3. 长按保持不重复]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},
            {true,  700'000},   // 触发 LongPressed
            {true,  800'000},   // 继续保持
            {true,  900'000},   // 继续保持
            {true, 1000'000},   // 继续保持 (不重复)
        });
        check(events.size() == 1, "只有 1 个 LongPressed (不重复)");
    }

    // ================================================================
    // 4. 长按释放不补 Pressed
    // ================================================================
    std::cout << "\n[4. 长按释放不补 Pressed]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},
            {true,  700'000},   // LongPressed
            {false, 800'000},   // 释放
            {false, 900'000},   // 释放稳定 → 不应输出 Pressed
        });
        check(events.size() == 1, "只有 1 个事件");
        check(events[0] == EncoderEvent::LongPressed, "是 LongPressed, 不是 Pressed");
    }

    // ================================================================
    // 5. press bounce 不触发
    // ================================================================
    std::cout << "\n[5. press bounce 不触发]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            // 模拟按下抖动: 1ms 内翻转多次
            {true,  100'000},
            {false, 100'500},   // 0.5ms 释放 (抖动)
            {true,  101'000},   // 0.5ms 按下 (抖动)
            {false, 101'500},   // 0.5ms 释放 (抖动)
            {true,  102'000},   // 稳定按下
            // 等到消抖完成
            {true,  200'000},   // candidate 结算 → stable=pressed
            // 短按释放
            {false, 250'000},
            {false, 350'000},   // 释放稳定 → Pressed
        });
        check(events.size() == 1, "抖动后短按 → 1 Pressed");
        check(!events.empty() && events[0] == EncoderEvent::Pressed, "事件 = Pressed");
    }

    // ================================================================
    // 6. release bounce 只产生 1 次 Pressed
    // ================================================================
    std::cout << "\n[6. release bounce 只产生1次]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},   // 按下
            {true,  200'000},   // 稳定按下 (settle @110ms)
            {false, 300'000},   // 释放
            // 释放抖动: 1ms 内翻转
            {true,  300'500},
            {false, 301'000},
            {true,  301'500},
            {false, 302'000},
            // 最终稳定释放
            {false, 400'000},   // 释放稳定 → Pressed (仅一次)
        });
        check(events.size() == 1, "释放抖动后 → 恰好 1 Pressed");
        check(!events.empty() && events[0] == EncoderEvent::Pressed, "事件 = Pressed");
    }

    // ================================================================
    // 7. 连续 3 次短按 → 3 Pressed
    // ================================================================
    std::cout << "\n[7. 连续3次短按 → 3 Pressed]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        std::vector<EncoderEvent> all_events;

        // 第 1 次
        {
            auto ev = feed(dec, {
                {false, 0},
                {true,  100'000},
                {false, 150'000},
                {false, 250'000},   // settle release → Pressed
            });
            all_events.insert(all_events.end(), ev.begin(), ev.end());
        }
        // 第 2 次
        {
            auto ev = feed(dec, {
                {true,  300'000},
                {false, 350'000},
                {false, 450'000},   // settle release → Pressed
            });
            all_events.insert(all_events.end(), ev.begin(), ev.end());
        }
        // 第 3 次
        {
            auto ev = feed(dec, {
                {true,  500'000},
                {false, 550'000},
                {false, 650'000},   // settle release → Pressed
            });
            all_events.insert(all_events.end(), ev.begin(), ev.end());
        }

        check(all_events.size() == 3, "3 次短按 → 3 个事件");
        for (auto& e : all_events) {
            check(e == EncoderEvent::Pressed, "每个事件 = Pressed");
        }
    }

    // ================================================================
    // 8. reset 后可正常短按
    // ================================================================
    std::cout << "\n[8. reset 后可正常短按]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);

        // 先做一次操作
        dec.update(false, 0);
        dec.update(true, 100'000);
        dec.update(false, 200'000);

        // reset
        dec.reset();

        // 全新短按
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},
            {false, 150'000},
            {false, 250'000},
        });
        check(events.size() == 1, "reset 后短按 → 1 Pressed");
        check(!events.empty() && events[0] == EncoderEvent::Pressed, "事件 = Pressed");
    }

    // ================================================================
    // 9. 稀疏采样下短按仍正确
    // ================================================================
    std::cout << "\n[9. 稀疏采样下短按仍正确]\n";
    {
        ButtonDecoder dec(LONG_US, DB_US);
        auto events = feed(dec, {
            {false, 0},
            {true,  100'000},   // 按下
            {false, 150'000},   // 50ms 后释放 (稀疏: 中间无采样)
            // 下一条采样在 150ms 后
            {false, 300'000},   // 释放结算 → Pressed
        });
        check(events.size() == 1, "稀疏采样短按 → 1 Pressed");
        check(!events.empty() && events[0] == EncoderEvent::Pressed, "事件 = Pressed");
    }

    // ================================================================
    // 10. 不同轮询间隔不改变长按起点语义
    // ================================================================
    std::cout << "\n[10. 不同轮询间隔 → 长按起点一致]\n";
    {
        // 场景 A: 密集轮询
        ButtonDecoder decA(LONG_US, DB_US);
        auto evA = feed(decA, {
            {false, 0},
            {true,  100'000},
            {true,  120'000},   // 20ms 后采样 (密集)
            {true,  140'000},
            {true,  700'000},   // 超阈值 → LongPressed
            {false, 800'000},
            {false, 900'000},
        });

        // 场景 B: 稀疏轮询
        ButtonDecoder decB(LONG_US, DB_US);
        auto evB = feed(decB, {
            {false, 0},
            {true,  100'000},
            // 跳过中间采样
            {true,  400'000},   // 300ms 后首次采样 (稀疏)
            {true,  700'000},   // 超阈值 → LongPressed
            {false, 800'000},
            {false, 900'000},
        });

        check(evA.size() == 1 && evA[0] == EncoderEvent::LongPressed,
              "场景A: LongPressed");
        check(evB.size() == 1 && evB[0] == EncoderEvent::LongPressed,
              "场景B(稀疏): 同样 LongPressed");

        // 关键: 两种场景的 press_start 都应该是 110ms (100 + 10 debounce)
        // 在 t=700ms 都应该触发 LongPressed (700 >= 110+500=610)
    }

    // ---- 结果 ----
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed << " 失败 ===\n";
    return (g_failed == 0) ? 0 : 1;
}
