// test_local_feature_service.cpp — R8_R4 §10.2 本地功能服务确定性测试。
// 注入时钟与临时目录: 定时器/闹钟/便签/显示模式/持久化与损坏恢复/上限。
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "features/local_feature_service.hpp"

using namespace holopet;

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

int main() {
    std::cout << "=== test_local_feature_service (R8_R4) ===\n\n";

    std::filesystem::path tmp = std::filesystem::temp_directory_path()
        / "holopet_r4_feat_test";
    std::filesystem::create_directories(tmp);

    {
        // 1. 定时器: 创建/列表/到期一次/取消/上限
        LocalFeatureService svc;
        std::string id;
        auto e = svc.createTimer(30000, "tea", 1000, 0, &id);
        check(e.ok && !id.empty(), "createTimer 成功");
        check(svc.listTimers().size() == 1, "列表含 1 个定时器");
        auto fires = svc.tick(20000, 0);
        check(fires.empty(), "未到期不触发");
        fires = svc.tick(31000, 0);
        check(fires.size() == 1 && fires[0].kind == "timer"
              && fires[0].id == id, "30s 到期恰好触发一次");
        fires = svc.tick(40000, 0);
        check(fires.empty(), "到期后不重复触发");
        check(svc.cancelTimer(id).ok, "取消成功");
        check(svc.listTimers().empty(), "取消后列表空");
        check(!svc.cancelTimer("t999").ok, "取消不存在 → not_found");
        for (int i = 0; i < LocalFeatureService::kMaxTimers; ++i)
            svc.createTimer(1000, "", 0);
        check(!svc.createTimer(1000, "", 0).ok, "定时器上限 8 → limit_exceeded");
    }

    {
        // 2. 闹钟: 一次性/每日/工作日/启停/删除/时区本地
        LocalFeatureService svc;
        std::string a1;
        int64_t day = 24 * 3600 * 1000LL;
        int64_t t0 = 2 * day + 8 * 3600 * 1000LL;   // 第3天 08:00 本地
        check(svc.createAlarm(7, 30, 0, "wake", t0, &a1).ok, "一次性闹钟创建");
        auto fires = svc.tick(0, t0);                 // 当天 08:00, 距次日 7:30 未到
        check(fires.empty(), "未到点不触发");
        fires = svc.tick(0, t0 + 20 * 3600 * 1000LL);  // 当天 7:30 已过 → 次日
        check(fires.empty(), "一次性到期前不触发");
        // 精确触发: next_fire = 当天 7:30 或次日 7:30
        auto a = svc.listAlarms()[0];
        fires = svc.tick(0, a.next_fire_wall_ms);
        check(fires.size() == 1 && fires[0].kind == "alarm", "一次性闹钟到点触发");
        check(!svc.listAlarms()[0].enabled, "一次性触发后自动失效");
        std::string a2;
        svc.createAlarm(7, 30, 1, "daily", t0, &a2);   // 每日
        auto d = svc.listAlarms().back();
        fires = svc.tick(0, d.next_fire_wall_ms);
        check(fires.size() == 1, "每日闹钟触发");
        check(svc.listAlarms().back().enabled, "每日闹钟触发后仍启用");
        check(svc.enableAlarm(a2, false).ok, "停用成功");
        fires = svc.tick(0, svc.listAlarms().back().next_fire_wall_ms);
        check(fires.empty(), "停用后不触发");
        check(svc.deleteAlarm(a2).ok, "删除成功");
        check(!svc.createAlarm(30, 0, 0, "", 0).ok, "越界分钟 → invalid_arg");
        check(!svc.createAlarm(7, 30, 5, "", 0).ok, "非法 repeat → invalid_arg");
        // 工作日闹钟: 周六不排期
        std::string a3;
        svc.createAlarm(9, 0, 2, "weekday", t0, &a3);
        auto wd = svc.listAlarms().back();
        int64_t dow = ((wd.next_fire_wall_ms / day) + 4) % 7;   // 0=周日
        check(dow >= 1 && dow <= 5, "工作日闹钟下次触发在周一~周五");
    }

    {
        // 3. 便签: UTF-8/上限/截断/增删读列
        LocalFeatureService svc;
        std::string n1;
        check(svc.addNote("你好, 世界 ☕", 100, &n1).ok, "UTF-8 便签创建");
        check(svc.addNote("second", 200).ok, "第二条");
        auto list = svc.listNotes(1);
        check(list.size() == 1 && list[0].text == "second", "list 限制+新到旧");
        std::string txt;
        check(svc.readNote(n1, &txt) && txt == "你好, 世界 ☕", "read 原文一致");
        check(svc.deleteNote(n1).ok, "删除指定 ID");
        check(!svc.deleteNote(n1).ok, "重复删除 → not_found");
        std::string over(501, 'a');
        check(!svc.addNote(over, 300).ok, "超过 500 字符 → invalid_arg");
        check(svc.addNote("", 300).ok == false, "空文本 → invalid_arg");
        for (int i = 0; i < LocalFeatureService::kMaxNotes; ++i)
            svc.addNote("n" + std::to_string(i), 400);
        check(!svc.addNote("overflow", 500).ok, "100 条上限 → limit_exceeded");
    }

    {
        // 4. 显示模式: 切换 + 持久化 + 重启恢复
        LocalFeatureService svc(tmp / "store.json");
        svc.setDisplayMode(DisplayMode::Clock);
        check(svc.save().ok, "保存成功");
        LocalFeatureService svc2(tmp / "store.json");
        check(svc2.load().ok, "加载成功");
        check(svc2.displayMode() == DisplayMode::Clock, "重启恢复 Clock 模式");
        // R8_R4_R3_R4: AI 模式持久化 (auto/fast/deep/local; 非法值回 auto)
        check(svc2.aiMode() == "auto", "缺省 ai_mode=auto");
        svc2.setAiMode("deep");
        check(svc2.aiMode() == "deep", "设置 deep");
        svc2.save();
        LocalFeatureService svc2b(tmp / "store.json");
        check(svc2b.load().ok, "ai_mode 加载成功");
        check(svc2b.aiMode() == "deep", "重启恢复 ai_mode=deep");
        svc2b.setAiMode("hack");
        check(svc2b.aiMode() == "auto", "非法 ai_mode 收敛为 auto");
        // 定时器重启恢复 (目标墙钟重算)
        LocalFeatureService svc3(tmp / "store2.json");
        std::string tid;
        svc3.createTimer(60000, "reboot", 5000, 100000, &tid);
        svc3.save();
        LocalFeatureService svc4(tmp / "store2.json");
        svc4.load(20000, 120000);     // 重启后: 单调 20000, 墙钟 120000
        auto rt = svc4.listTimers();
        check(rt.size() == 1, "定时器持久化恢复");
        // 目标墙钟 100000+60000=160000; 现墙 120000 → 余 40000 → 单调 60000 到期
        auto fires = svc4.tick(60000, 160000);
        check(fires.size() == 1, "恢复后按重算到期触发");
    }

    {
        // 5. 损坏文件恢复 (保留副本 + 空安全状态 + 明确短码)
        auto bad = tmp / "bad.json";
        {
            std::ofstream f(bad, std::ios::binary);
            f << "{\"schema_version\":1,\"display\":\"pet\",\"timers\":[broken";
        }
        LocalFeatureService svc(bad);
        auto e = svc.load(0, 0);
        check(!e.ok && e.code == "corrupt_recovered", "损坏 → corrupt_recovered");
        check(svc.listTimers().empty() && svc.listNotes(0).empty(),
              "损坏回空安全状态");
        check(std::filesystem::exists(bad.string() + ".corrupt"),
              "损坏副本保留为 .corrupt");
    }

    std::filesystem::remove_all(tmp);
    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? 0 : 1;
}
