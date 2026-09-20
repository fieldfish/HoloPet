// test_menu_controller.cpp — R8_R4 §10.2 EC11 菜单状态机确定性测试。
#include <iostream>
#include <string>

#include "ui/menu_controller.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_menu_controller (R8_R4) ===\n\n";

    {
        // 1. Idle 短按=录音开始; 长按=进入菜单; 短按/长按互斥
        MenuController m;
        check(m.handle(EncoderEvent::Pressed, false, 0)
              == MenuAction::StartRecording, "Idle 短按 → 开始录音");
        check(m.handle(EncoderEvent::LongPressed, false, 100)
              == MenuAction::EnterMenu, "Idle 长按 → 进入菜单");
        check(m.snapshot().active && m.snapshot().layer == MenuLayer::Root,
              "菜单已激活 (根层)");
        check(m.handle(EncoderEvent::Pressed, false, 200)
              == MenuAction::ShowClock,
              "根层短按进入当前项 (idx 0 时钟 → ShowClock)");
        check(!m.snapshot().active, "进入项目后退出菜单 (菜单非默认常驻)");
    }

    {
        // 2. 旋转步进 (方向一致, 循环) + 根层选项确认
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        m.handle(EncoderEvent::Clockwise, false, 10);
        check(m.snapshot().selected_index == 1, "顺转 → 选择 1 (定时器)");
        m.handle(EncoderEvent::Clockwise, false, 20);
        check(m.snapshot().selected_index == 2, "顺转 → 选择 2 (闹钟)");
        m.handle(EncoderEvent::Clockwise, false, 30);
        check(m.snapshot().selected_index == 3, "顺转 → 选择 3 (便签)");
        m.handle(EncoderEvent::Clockwise, false, 40);
        check(m.snapshot().selected_index == 4, "顺转 → 选择 4 (AI模式)");
        m.handle(EncoderEvent::Clockwise, false, 50);
        check(m.snapshot().selected_index == 0, "顺转循环回 0 (时钟)");
        m.handle(EncoderEvent::CounterClockwise, false, 60);
        check(m.snapshot().selected_index == 4, "逆转 → 选择 4 (AI模式)");
    }

    {
        // 3. 定时器编辑: 旋转改值 + 短按确认 + 长按取消不保存
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        m.handle(EncoderEvent::Clockwise, false, 5);         // 选 1=定时器
        m.handle(EncoderEvent::Pressed, false, 10);          // 进定时器列表
        check(m.snapshot().layer == MenuLayer::TimerList, "进入定时器列表");
        m.handle(EncoderEvent::Pressed, false, 15);          // 首项=新建
        check(m.snapshot().layer == MenuLayer::TimerCreate, "新建 → 定时器编辑");
        for (int i = 0; i < 5; ++i)
            m.handle(EncoderEvent::Clockwise, false, 20 + i);
        check(m.snapshot().edit_minutes == 15, "旋转加 5 分钟 → 15");
        check(m.handle(EncoderEvent::Pressed, false, 100)
              == MenuAction::CreateTimer, "短按确认 → CreateTimer(15)");
        check(m.snapshot().edit_minutes == 15, "确认后值可读");
        // 再进编辑, 长按取消: 值可改但不产出 CreateTimer
        m.handle(EncoderEvent::Pressed, false, 200);
        m.handle(EncoderEvent::Clockwise, false, 210);
        check(m.handle(EncoderEvent::LongPressed, false, 220)
              == MenuAction::None, "长按取消编辑不产出创建");
        check(m.snapshot().layer == MenuLayer::Root, "取消后回根层");
    }

    {
        // 4. 闹钟编辑链: 时→分→重复, 越界回绕
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        m.handle(EncoderEvent::Clockwise, false, 8);         // 选 2=闹钟
        m.handle(EncoderEvent::Clockwise, false, 10);
        m.handle(EncoderEvent::Pressed, false, 20);          // 进闹钟列表
        check(m.snapshot().layer == MenuLayer::AlarmList, "进入闹钟列表");
        m.handle(EncoderEvent::Pressed, false, 25);          // 首项=新建
        check(m.snapshot().layer == MenuLayer::AlarmCreateHour, "新建 → 时编辑");
        for (int i = 0; i < 30; ++i)
            m.handle(EncoderEvent::Clockwise, false, 30 + i);
        check(m.snapshot().edit_hour == 13, "时从 7 加 30 回绕 → 13");
        m.handle(EncoderEvent::Pressed, false, 100);         // 确认时 → 分
        check(m.snapshot().layer == MenuLayer::AlarmCreateMinute, "进分编辑");
        m.handle(EncoderEvent::CounterClockwise, false, 110);
        check(m.snapshot().edit_minute == 29, "分减 1 → 29");
        m.handle(EncoderEvent::Pressed, false, 120);         // 确认分 → 重复
        check(m.snapshot().layer == MenuLayer::AlarmCreateRepeat, "进重复编辑");
        m.handle(EncoderEvent::Clockwise, false, 130);
        check(m.snapshot().edit_repeat == 1, "重复 → 每日");
        check(m.handle(EncoderEvent::Pressed, false, 140)
              == MenuAction::CreateAlarm, "确认 → CreateAlarm(7:29 每日)");
    }

    {
        // 5. 时钟切换: 根层选时钟 → ShowClock; 再长按退出恢复宠物
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        check(m.handle(EncoderEvent::Pressed, false, 30)
              == MenuAction::ShowClock, "选时钟 (idx 0) → ShowClock");
        check(!m.snapshot().active, "切换后退出菜单");
        check(m.handle(EncoderEvent::LongPressed, false, 100)
              == MenuAction::EnterMenu, "Clock 下长按仍可进菜单");
        check(m.handle(EncoderEvent::Pressed, false, 130)
              == MenuAction::ShowPet, "再选时钟 → ShowPet 切换回宠物");
    }

    {
        // 6. busy 状态: 旋转/短按忽略; 长按=紧急取消
        MenuController m;
        check(m.handle(EncoderEvent::Clockwise, true, 0) == MenuAction::None,
              "busy 旋转忽略");
        check(m.handle(EncoderEvent::Pressed, true, 10) == MenuAction::None,
              "busy 短按忽略");
        check(m.handle(EncoderEvent::LongPressed, true, 20)
              == MenuAction::EmergencyCancel, "busy 长按 → 紧急取消");
        check(!m.snapshot().active, "busy 长按不进入菜单");
    }

    {
        // 7. 菜单 15s 无操作自动退出
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        check(m.snapshot().active, "菜单激活");
        check(m.handle(EncoderEvent::Clockwise, false, 15100)
              == MenuAction::ExitMenu, "15s 无操作 → 自动退出");
        check(!m.snapshot().active, "已退出菜单");
    }

    {
        // 8. 长按返回层级: 编辑页长按取消 → 根层; 根层长按 → 退出
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        m.handle(EncoderEvent::Clockwise, false, 5);         // 选 1=定时器
        m.handle(EncoderEvent::Pressed, false, 10);          // 定时器列表
        m.handle(EncoderEvent::Pressed, false, 15);          // 新建 → 编辑
        m.handle(EncoderEvent::LongPressed, false, 20);      // 编辑长按 → 根层
        check(m.snapshot().layer == MenuLayer::Root, "编辑页长按回根层");
        check(m.handle(EncoderEvent::LongPressed, false, 30)
              == MenuAction::ExitMenu, "根层长按 → 退出菜单");
        check(!m.snapshot().active, "根层长按后退出");
    }

    {
        // 9. R8_R4_R3_R4: AI 模式页 — 进入/选择/确认/长按取消不保存
        MenuController m;
        m.handle(EncoderEvent::LongPressed, false, 0);
        m.handle(EncoderEvent::Clockwise, false, 5);
        m.handle(EncoderEvent::Clockwise, false, 10);
        m.handle(EncoderEvent::Clockwise, false, 15);
        m.handle(EncoderEvent::Clockwise, false, 20);        // 选 4=AI模式
        m.handle(EncoderEvent::Pressed, false, 25);
        check(m.snapshot().layer == MenuLayer::AiMode, "进入 AI 模式页");
        check(m.snapshot().items.size() == 4
              && m.snapshot().items[0] == "快速"
              && m.snapshot().items[3] == "本地", "四项: 快速/自动/深度/本地");
        m.handle(EncoderEvent::Clockwise, false, 30);
        m.handle(EncoderEvent::Clockwise, false, 40);
        check(m.handle(EncoderEvent::Pressed, false, 50)
              == MenuAction::SetAiMode, "短按确认 → SetAiMode");
        check(m.snapshot().action_index == 2, "选择 2=深度");
        check(m.snapshot().layer == MenuLayer::Root, "确认后回根层");
        check(m.snapshot().selected_index == 4, "确认后停在 AI模式 项");
        // 再进, 长按返回不保存
        m.handle(EncoderEvent::Pressed, false, 95);
        check(m.snapshot().layer == MenuLayer::AiMode, "再进 AI 模式页");
        m.handle(EncoderEvent::Clockwise, false, 100);
        check(m.handle(EncoderEvent::LongPressed, false, 110)
              == MenuAction::None, "长按返回不产出 SetAiMode");
        check(m.snapshot().layer == MenuLayer::Root, "长按后回根层");
        check(m.snapshot().action_index == -1, "未确认不携带 action_index");
    }

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
