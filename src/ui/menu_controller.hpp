#pragma once
/**
 * menu_controller.hpp — R8_R4 EC11 旋钮菜单状态机 (纯 C++, 无 SDL)。
 *
 * §4 契约:
 *   Idle/Clock 短按 → StartRecording / StopRecording (由 busy 决定);
 *   Idle/Clock 长按 → 进入功能菜单 (根菜单);
 *   Menu 旋转 → 逐项移动; 短按 → 确认/进入; 长按 → 返回/退出;
 *   编辑页旋转 → 改值; 短按 → 确认字段; 长按 → 取消编辑;
 *   Recording/Thinking/Speaking 长按 → EmergencyCancel (不进入菜单);
 *   busy 时旋转/短按一律忽略 (防误改);
 *   菜单 15s 无操作自动退出到进入前画面; 退出恢复 Pet/Clock。
 *
 * 本状态机只做"菜单导航 + 事件语义"判定; 对 LocalFeatureService 的调用由
 * 上层把 Action 映射到同一服务实例 (旋钮与语音共用, 不复制规则)。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "input/input_event.hpp"

namespace holopet {

enum class MenuAction {
    None,
    StartRecording,
    StopRecording,
    EmergencyCancel,
    EnterMenu,
    ExitMenu,
    ShowClock,
    ShowPet,
    CreateTimer,        // 参数在 snapshot().edit_minutes
    CancelTimer,        // 参数在 snapshot().selected_index (定时器列表)
    CreateAlarm,        // 参数: edit_hour/edit_minute/edit_repeat
    DeleteAlarm,
    ToggleAlarm,
    ReadNote,           // 参数: selected_index (便签列表)
    DeleteNote,
    SetAiMode,          // R8_R4_R3_R4: 参数 action_index (0快速 1自动 2深度 3本地)
};

enum class MenuLayer {
    Inactive,           // 宠物/时钟画面
    Root,               // [定时器, 闹钟, 时钟, 便签]
    TimerCreate,        // 编辑分钟数 (1..120)
    TimerList,          // 列表: 选择取消
    AlarmCreateHour,
    AlarmCreateMinute,
    AlarmCreateRepeat,  // 一次性/每日/工作日
    AlarmList,          // 列表: 删除/启停 (短按删除, 长按启停)
    NoteList,           // 列表: 短按读, 长按删
    AiMode,             // R8_R4_R3_R4: AI模式选择 (快速/自动/深度/本地)
};

struct MenuSnapshot {
    MenuLayer layer = MenuLayer::Inactive;
    bool active = false;
    int selected_index = 0;
    int edit_minutes = 10;
    int edit_hour = 7;
    int edit_minute = 30;
    int edit_repeat = 0;        // 0 一次性 1 每日 2 工作日
    int action_index = -1;      // R8_R4_R3_R1: 携带动作的目标下标 (列表项)
    std::string title;
    std::vector<std::string> items;
    std::string preview_text;   // R8_R4_R3_R1: 便签阅读预览
};

/** 菜单时序参数 (顶层: GCC 14 不得在类内用 NSDMI 作默认参数)。 */
struct MenuTiming {
    int64_t idle_timeout_ms = 15000;   // §4: 菜单 15s 无操作退出
};

class MenuController {
public:
    explicit MenuController(MenuTiming t = MenuTiming()) : t_(t) {}

    /** 单一入口: 一个物理事件只消费一次; 返回对应动作。 */
    MenuAction handle(EncoderEvent ev, bool busy, int64_t now_ms) {
        // 空闲超时 (Inactive 无操作不计数; 只在菜单层)
        if (layer_ != MenuLayer::Inactive
            && now_ms - last_op_ms_ >= t_.idle_timeout_ms) {
            exitToPrev();
            return MenuAction::ExitMenu;
        }
        if (busy) {
            // 忙状态: 只有长按=紧急取消; 其余一律忽略 (旋转不误改)
            if (ev == EncoderEvent::LongPressed) {
                last_op_ms_ = now_ms;
                return MenuAction::EmergencyCancel;
            }
            return MenuAction::None;
        }
        switch (layer_) {
        case MenuLayer::Inactive:
            if (ev == EncoderEvent::Pressed) {
                last_op_ms_ = now_ms;
                return MenuAction::StartRecording;
            }
            if (ev == EncoderEvent::LongPressed) {
                enterRoot(now_ms);
                return MenuAction::EnterMenu;
            }
            return MenuAction::None;
        case MenuLayer::Root:
            return handleRoot(ev, now_ms);
        case MenuLayer::TimerCreate:
            return handleTimerCreate(ev, now_ms);
        case MenuLayer::TimerList:
            return handleTimerList(ev, now_ms);
        case MenuLayer::AlarmCreateHour:
        case MenuLayer::AlarmCreateMinute:
        case MenuLayer::AlarmCreateRepeat:
            return handleAlarmEdit(ev, now_ms);
        case MenuLayer::AlarmList:
            return handleAlarmList(ev, now_ms);
        case MenuLayer::NoteList:
            return handleNoteList(ev, now_ms);
        case MenuLayer::AiMode:
            return handleAiMode(ev, now_ms);
        }
        return MenuAction::None;
    }

    MenuSnapshot snapshot() const {
        MenuSnapshot s;
        s.layer = layer_;
        s.active = layer_ != MenuLayer::Inactive;
        s.selected_index = idx_;
        s.edit_minutes = edit_minutes_;
        s.edit_hour = edit_hour_;
        s.edit_minute = edit_minute_;
        s.edit_repeat = edit_repeat_;
        s.title = title_;
        s.items = items_;
        s.action_index = action_index_;
        s.preview_text = preview_text_;
        return s;
    }

    /** R8_R4_R3_R1: 主循环注入列表数据 (便签/定时器/闹钟 摘要);
     *  下标越界收敛, 不改变当前层。 */
    void syncItems(const std::vector<std::string>& v) {
        items_ = v;
        if (items_.empty()) idx_ = 0;
        else if (idx_ >= static_cast<int>(items_.size()))
            idx_ = static_cast<int>(items_.size()) - 1;
    }

    /** R8_R4_R3_R1: 便签正文预览 (ReadNote 后由主循环注入)。 */
    void setPreviewText(const std::string& t) { preview_text_ = t; }

    /** R8_R4_R3_R4: 设置当前项下标 (进入 AI 模式页时同步持久化选项)。 */
    void setIndex(int i) {
        if (!items_.empty() && i >= 0
            && i < static_cast<int>(items_.size())) idx_ = i;
    }

private:
    void setItems(const std::vector<std::string>& v) {
        items_ = v;
        if (idx_ >= static_cast<int>(v.size())) idx_ = 0;
    }

    void enterRoot(int64_t now_ms) {
        prev_was_clock_ = show_clock_;
        layer_ = MenuLayer::Root;
        idx_ = 0;
        title_ = "功能";
        setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
        preview_text_.clear();
        action_index_ = -1;
        last_op_ms_ = now_ms;
    }

    void exitToPrev() {
        layer_ = MenuLayer::Inactive;
        idx_ = 0;
        title_.clear();
        items_.clear();
        preview_text_.clear();
    }

    MenuAction handleRoot(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        const int n = static_cast<int>(items_.size());
        if (ev == EncoderEvent::Clockwise) {
            idx_ = n ? (idx_ + 1) % n : 0;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::CounterClockwise) {
            idx_ = n ? (idx_ + n - 1) % n : 0;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            switch (idx_) {
            case 0:   // 时钟
                show_clock_ = !show_clock_;
                exitToPrev();
                return show_clock_ ? MenuAction::ShowClock
                                   : MenuAction::ShowPet;
            case 1:
                // R8_R4_R3_R4_R5_R2: 列表优先 — 首项"新建", 其余为真实定时器
                layer_ = MenuLayer::TimerList;
                title_ = "定时器";
                idx_ = 0;
                setItems({});
                break;
            case 2:
                layer_ = MenuLayer::AlarmList;
                title_ = "闹钟";
                idx_ = 0;
                setItems({});
                break;
            case 3:
                layer_ = MenuLayer::NoteList;
                title_ = "便签";
                setItems({});
                break;
            case 4:
                layer_ = MenuLayer::AiMode;
                title_ = "AI模式";
                setItems({"快速", "自动", "深度", "本地"});
                break;
            }
            return MenuAction::None;
        }
        if (ev == EncoderEvent::LongPressed) {
            exitToPrev();
            return MenuAction::ExitMenu;
        }
        return MenuAction::None;
    }

    // R8_R4_R3_R4: AI模式页 — 旋转选择, 短按确认 (SetAiMode) 回根, 长按返回不保存
    MenuAction handleAiMode(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        const int n = static_cast<int>(items_.size());
        if (ev == EncoderEvent::Clockwise) {
            idx_ = n ? (idx_ + 1) % n : 0;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::CounterClockwise) {
            idx_ = n ? (idx_ + n - 1) % n : 0;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            action_index_ = idx_;
            layer_ = MenuLayer::Root;
            idx_ = 4;
            title_ = "功能";
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            return MenuAction::SetAiMode;
        }
        if (ev == EncoderEvent::LongPressed) {
            action_index_ = -1;
            layer_ = MenuLayer::Root;
            idx_ = 4;
            title_ = "功能";
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            return MenuAction::None;   // 不保存
        }
        return MenuAction::None;
    }

    MenuAction handleTimerCreate(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        if (ev == EncoderEvent::Clockwise) {
            edit_minutes_ = edit_minutes_ >= 120 ? 120 : edit_minutes_ + 1;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::CounterClockwise) {
            edit_minutes_ = edit_minutes_ <= 1 ? 1 : edit_minutes_ - 1;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            layer_ = MenuLayer::Root;
            idx_ = 1;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return MenuAction::CreateTimer;
        }
        if (ev == EncoderEvent::LongPressed) {
            layer_ = MenuLayer::Root;
            idx_ = 1;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return MenuAction::None;   // 取消本次编辑, 不保存
        }
        return MenuAction::None;
    }

    MenuAction handleTimerList(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        if (ev == EncoderEvent::Clockwise || ev == EncoderEvent::CounterClockwise) {
            const int nn = static_cast<int>(items_.size());
            if (nn) idx_ = (idx_ + (ev == EncoderEvent::Clockwise ? 1 : nn - 1))
                           % nn;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            if (idx_ == 0) {
                // 新建: 进入分钟编辑
                layer_ = MenuLayer::TimerCreate;
                title_ = "定时器 (分钟)";
                edit_minutes_ = 10;
                setItems({});
                return MenuAction::None;
            }
            action_index_ = idx_ - 1;   // 列表项 → 定时器下标 (0 起)
            return MenuAction::CancelTimer;
        }
        if (ev == EncoderEvent::LongPressed) {
            layer_ = MenuLayer::Root;
            idx_ = 1;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return MenuAction::None;
        }
        return MenuAction::None;
    }

    MenuAction handleAlarmEdit(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        int lo = (layer_ == MenuLayer::AlarmCreateHour) ? 0
               : (layer_ == MenuLayer::AlarmCreateMinute) ? 0 : 0;
        int hi = (layer_ == MenuLayer::AlarmCreateHour) ? 23
               : (layer_ == MenuLayer::AlarmCreateMinute) ? 59 : 2;
        if (ev == EncoderEvent::Clockwise || ev == EncoderEvent::CounterClockwise) {
            int d = ev == EncoderEvent::Clockwise ? 1 : -1;
            int v = (layer_ == MenuLayer::AlarmCreateHour) ? edit_hour_
                  : (layer_ == MenuLayer::AlarmCreateMinute) ? edit_minute_
                  : edit_repeat_;
            v += d;
            if (v < lo) v = hi;
            if (v > hi) v = lo;
            if (layer_ == MenuLayer::AlarmCreateHour) edit_hour_ = v;
            else if (layer_ == MenuLayer::AlarmCreateMinute) edit_minute_ = v;
            else edit_repeat_ = v;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            if (layer_ == MenuLayer::AlarmCreateHour)
                layer_ = MenuLayer::AlarmCreateMinute;
            else if (layer_ == MenuLayer::AlarmCreateMinute)
                layer_ = MenuLayer::AlarmCreateRepeat;
            else {
                layer_ = MenuLayer::Root;
                idx_ = 2;
                setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
                title_ = "功能";
                return MenuAction::CreateAlarm;
            }
            title_ = (layer_ == MenuLayer::AlarmCreateMinute) ? "闹钟-分"
                   : (layer_ == MenuLayer::AlarmCreateRepeat) ? "闹钟-重复"
                   : "闹钟-时";
            return MenuAction::None;
        }
        if (ev == EncoderEvent::LongPressed) {
            layer_ = MenuLayer::Root;
            idx_ = 2;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return MenuAction::None;
        }
        return MenuAction::None;
    }

    MenuAction handleAlarmList(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        if (ev == EncoderEvent::Clockwise || ev == EncoderEvent::CounterClockwise) {
            const int nn = static_cast<int>(items_.size());
            if (nn) idx_ = (idx_ + (ev == EncoderEvent::Clockwise ? 1 : nn - 1))
                           % nn;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            if (idx_ == 0) {
                // 新建: 进入时编辑
                layer_ = MenuLayer::AlarmCreateHour;
                title_ = "闹钟-时";
                edit_hour_ = 7;
                edit_minute_ = 30;
                edit_repeat_ = 0;
                setItems({});
                return MenuAction::None;
            }
            action_index_ = idx_ - 1;
            return MenuAction::DeleteAlarm;
        }
        if (ev == EncoderEvent::LongPressed) {
            if (idx_ == 0) {
                layer_ = MenuLayer::Root;
                idx_ = 2;
                setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
                title_ = "功能";
                return MenuAction::None;
            }
            action_index_ = idx_ - 1;
            MenuAction a = MenuAction::ToggleAlarm;
            layer_ = MenuLayer::Root;
            idx_ = 2;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return a;
        }
        return MenuAction::None;
    }

    MenuAction handleNoteList(EncoderEvent ev, int64_t now_ms) {
        last_op_ms_ = now_ms;
        if (ev == EncoderEvent::Clockwise || ev == EncoderEvent::CounterClockwise) {
            const int nn = static_cast<int>(items_.size());
            if (nn) idx_ = (idx_ + (ev == EncoderEvent::Clockwise ? 1 : nn - 1))
                           % nn;
            return MenuAction::None;
        }
        if (ev == EncoderEvent::Pressed) {
            action_index_ = idx_;
            return MenuAction::ReadNote;
        }
        if (ev == EncoderEvent::LongPressed) {
            action_index_ = idx_;
            MenuAction a = MenuAction::DeleteNote;
            layer_ = MenuLayer::Root;
            idx_ = 3;
            setItems({"时钟", "定时器", "闹钟", "便签", "AI模式"});
            title_ = "功能";
            return a;
        }
        return MenuAction::None;
    }

    MenuTiming t_;
    MenuLayer layer_ = MenuLayer::Inactive;
    bool show_clock_ = false;
    bool prev_was_clock_ = false;
    int idx_ = 0;
    int edit_minutes_ = 10;
    int edit_hour_ = 7;
    int edit_minute_ = 30;
    int edit_repeat_ = 0;
    std::string title_;
    std::vector<std::string> items_;
    int action_index_ = -1;
    std::string preview_text_;
    int64_t last_op_ms_ = 0;
};

} // namespace holopet
