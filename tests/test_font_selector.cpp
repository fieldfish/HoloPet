// tests/test_font_selector.cpp — R8_R2_R7 CJK 字形选择行为测试 (纯逻辑, 无 SDL 依赖)。
//
// 覆盖字体选择器的 9 项行为要求:
//   1 DejaVu 可开但无 CJK、Droid 可开且有 CJK → 必须选 Droid
//   2 Droid 缺失、Noto 有 CJK → 选 Noto
//   3 首个候选可开但缺 你/中/文 任一 → 继续下一个 (3 个子用例)
//   4 全部候选无 CJK → font_cjk_unavailable, 不得返回成功
//   5 ASCII 与 CJK 都满足才可 cjk_capable=yes
//   6 未选中字体关闭一次; 选中字体交给外层 guard, 不能提前关闭
//   7 环境变量字体存在但 glyph 不足时不能强制通过
//   8 字体日志字段完整且不含用户主目录
//   9 (现有中文分页/UTF-8/审计/零 TTS 由回归套件覆盖, 本文件不重复实现)
#include "display/font_selector.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static int g_fail = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL: %s (line %d)\n", (msg), __LINE__);          \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

namespace {

using holopet::FontOps;
using holopet::FontProbeResult;
using holopet::FontSelection;

struct FakeFont {
    bool opened = false;
    bool ascii = false;
    bool cjk = false;
};

struct FakeWorld {
    std::map<std::string, FakeFont> fonts;
    std::map<std::string, int> close_count;
    int probe_calls = 0;

    FontOps ops() {
        FontOps o;
        o.probe = [this](const std::string& p) {
            ++probe_calls;
            FontProbeResult r;
            auto it = fonts.find(p);
            if (it == fonts.end() || !it->second.opened) return r;   // 打不开
            r.opened = true;
            r.ascii_ok = it->second.ascii;
            r.cjk_ok = it->second.cjk;
            // 句柄用路径指针的稳定副本地址表示 (仅内部一致性用)
            r.handle = const_cast<char*>(it->first.c_str());
            return r;
        };
        o.close = [this](void* h) {
            close_count[std::string(static_cast<const char*>(h))] += 1;
        };
        return o;
    }
};

// 记录"打开次数"的世界: 用 probe 计数代替句柄级打开计数
struct OpenCountWorld {
    std::map<std::string, FakeFont> fonts;
    std::map<std::string, int> closes;
    int opens = 0;

    FontOps ops() {
        FontOps o;
        o.probe = [this](const std::string& p) {
            ++opens;
            FontProbeResult r;
            auto it = fonts.find(p);
            if (it == fonts.end() || !it->second.opened) return r;
            r.opened = true;
            r.ascii_ok = it->second.ascii;
            r.cjk_ok = it->second.cjk;
            r.handle = const_cast<char*>(it->first.c_str());
            return r;
        };
        o.close = [this](void* h) {
            closes[std::string(static_cast<const char*>(h))] += 1;
        };
        return o;
    }
};

const char* kDejaVu = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
const char* kDroid = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";
const char* kNoto = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
const char* kWqy = "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";

// ---- 1) DejaVu(无CJK) + Droid(有CJK) → 选 Droid ----
void test_pick_droid_over_dejavu() {
    FakeWorld w;
    w.fonts[kDejaVu] = {true, true, false};
    w.fonts[kDroid] = {true, true, true};
    FontOps ops = w.ops();
    FontSelection s = holopet::selectFont({kDejaVu, kDroid}, ops);
    CHECK(s.cjk_capable, "1: 必须判定 cjk_capable");
    CHECK(s.path == kDroid, "1: 必须选中 Droid 而非 DejaVu");
    CHECK(s.reason == "selected", "1: reason=selected");
    CHECK(s.attempts.size() == 2 && s.attempts[0].closed_unselected,
          "1: DejaVu 未选中必须关闭");
    CHECK(w.close_count[kDejaVu] == 1, "1: DejaVu 关闭恰一次");
    CHECK(w.close_count.count(kDroid) == 0, "1: 选中字体不得提前关闭");
}

// ---- 2) Droid 不存在, Noto 有 CJK → 选 Noto ----
void test_pick_noto_when_droid_missing() {
    FakeWorld w;
    w.fonts[kNoto] = {true, true, true};
    FontOps ops = w.ops();
    FontSelection s = holopet::selectFont({kDroid, kNoto}, ops);
    CHECK(s.cjk_capable && s.path == kNoto, "2: 必须选中 Noto");
    CHECK(s.attempts[0].opened == false, "2: Droid 未打开");
}

// ---- 3) 首个候选可开但缺任一 CJK glyph → 继续 ----
void test_continue_on_missing_glyph() {
    // 3a: cjk 全缺
    {
        FakeWorld w;
        w.fonts[kDejaVu] = {true, true, false};
        w.fonts[kWqy] = {true, true, true};
        FontSelection s = holopet::selectFont({kDejaVu, kWqy}, w.ops());
        CHECK(s.path == kWqy, "3a: 缺 CJK 必须继续到下一个");
        CHECK(w.close_count[kDejaVu] == 1, "3a: 缺 glyph 的候选关闭一次");
    }
    // 3b: 只有部分 CJK (用仅含 你/中 的字体模拟: cjk_ok=false)
    {
        FakeWorld w;
        w.fonts[kDejaVu] = {true, true, true};   // 占位: 直接给合格
        w.fonts[kDroid] = {true, true, false};  // 部分字形 → 不合格
        FontSelection s = holopet::selectFont({kDroid, kDejaVu}, w.ops());
        CHECK(s.path == kDejaVu, "3b: 部分字形不合格必须继续");
        CHECK(w.close_count[kDroid] == 1, "3b: 部分字形候选关闭一次");
    }
    // 3c: ASCII 缺失也不合格
    {
        FakeWorld w;
        w.fonts[kDroid] = {true, false, true};   // 无 ASCII
        w.fonts[kNoto] = {true, true, true};
        FontSelection s = holopet::selectFont({kDroid, kNoto}, w.ops());
        CHECK(s.path == kNoto, "3c: 缺 ASCII 不合格必须继续");
    }
}

// ---- 4) 全部无 CJK → font_cjk_unavailable ----
void test_all_without_cjk_unavailable() {
    FakeWorld w;
    w.fonts[kDejaVu] = {true, true, false};
    FontSelection s = holopet::selectFont({kDejaVu}, w.ops());
    CHECK(!s.cjk_capable, "4: 不得判定可用");
    CHECK(s.reason == "font_cjk_unavailable", "4: reason 必须显式不可用");
    CHECK(s.handle == nullptr, "4: 不得返回句柄");
    CHECK(w.close_count[kDejaVu] == 1, "4: 全部不合格时逐个关闭");
}

// ---- 4b) Pi 场景: 唯一 CJK 字体缺 ASCII (Droid) 必须被选中为后备, 不得判不可用 ----
void test_cjk_only_font_selected_as_backup() {
    FakeWorld w;
    w.fonts[kDroid] = {true, false, true};   // 有 你/中/文, 无 ASCII 映射 (Pi 实测)
    FontSelection s = holopet::selectFont({kDroid}, w.ops());
    CHECK(s.reason == "selected", "4b: 仅 CJK 字体也必须选中 (Pi 主场景)");
    CHECK(s.status == "selected_cjk_only", "4b: 状态标记 selected_cjk_only");
    CHECK(!s.cjk_capable, "4b: 无 ASCII 不得标记 cjk_capable (§4.2)");
    CHECK(s.handle != nullptr, "4b: 必须返回句柄供渲染");
    CHECK(w.close_count.count(kDroid) == 0, "4b: 选中句柄不得提前关闭");
    const std::string line = holopet::formatFontLogLine(s, 22);
    CHECK(line.find("font_cjk_glyphs=yes") != std::string::npos,
          "4b: 日志必须记录 CJK glyphs=yes");
    CHECK(line.find("font_ascii_glyphs=no") != std::string::npos,
          "4b: 日志必须如实记录 ASCII=no");
    CHECK(line.find("font_status=selected_cjk_only") != std::string::npos,
          "4b: 日志必须记录状态");
}

// ---- 4c) 后备被双满足候选取代时, 后备句柄必须关闭恰一次 ----
void test_cjk_only_backup_replaced_by_full() {
    FakeWorld w;
    w.fonts[kDroid] = {true, false, true};   // 后备
    w.fonts[kNoto] = {true, true, true};     // 双满足 → 应胜出
    FontSelection s = holopet::selectFont({kDroid, kNoto}, w.ops());
    CHECK(s.path == kNoto && s.status == "selected_full",
          "4c: 双满足候选胜出");
    CHECK(s.cjk_capable, "4c: 双满足 → cjk_capable");
    CHECK(w.close_count[kDroid] == 1, "4c: 被取代的后备关闭恰一次");
    CHECK(w.close_count.count(kNoto) == 0, "4c: 胜出者不得关闭");
}

void test_ascii_and_cjk_required() {
    FakeWorld w;
    w.fonts[kNoto] = {true, true, false};   // 只有 ASCII
    CHECK(!holopet::selectFont({kNoto}, w.ops()).cjk_capable, "5: 仅 ASCII 不算可读");
    FakeWorld w2;
    w2.fonts[kNoto] = {true, false, true};  // 只有 CJK
    CHECK(!holopet::selectFont({kNoto}, w2.ops()).cjk_capable, "5: 仅 CJK 不算可读");
    FakeWorld w3;
    w3.fonts[kNoto] = {true, true, true};
    CHECK(holopet::selectFont({kNoto}, w3.ops()).cjk_capable, "5: 双满足才算可读");
}

// ---- 6) 关闭语义: 未选中恰一次, 选中零次 ----
void test_close_semantics() {
    OpenCountWorld w;
    w.fonts[kDejaVu] = {true, true, false};
    w.fonts[kDroid] = {true, true, true};
    FontSelection s = holopet::selectFont({kDejaVu, kDroid}, w.ops());
    CHECK(w.closes[kDejaVu] == 1, "6: 未选中关闭恰一次");
    CHECK(w.closes.count(kDroid) == 0, "6: 选中不得被关闭");
    CHECK(s.attempts[0].closed_unselected && s.attempts[1].kept == true,
          "6: attempt 标记正确");
    CHECK(w.opens == 2, "6: 两次探测打开");
}

// ---- 7) 环境变量字体存在但 glyph 不足 → 不得强制通过 ----
void test_env_font_without_glyphs_rejected() {
    const std::string env_font = "/tmp/user-provided-font.ttf";
    FakeWorld w;
    w.fonts[env_font] = {true, true, false};   // 可开但无 CJK
    w.fonts[kDroid] = {true, true, true};
    FontSelection s = holopet::selectFont({env_font, kDroid}, w.ops());
    CHECK(s.path == kDroid, "7: 环境变量字体不合格必须继续");
    CHECK(w.close_count[env_font] == 1, "7: 不合格的环境字体关闭一次");
    // 环境字体是唯一候选且不合格 → 不可用
    FakeWorld w2;
    w2.fonts[env_font] = {true, true, false};
    FontSelection s2 = holopet::selectFont({env_font}, w2.ops());
    CHECK(!s2.cjk_capable && s2.reason == "font_cjk_unavailable",
          "7: 唯一环境字体不合格时不得通过");
}

// ---- 8) 日志字段完整且不含用户主目录 ----
void test_log_fields_and_privacy() {
    FakeWorld w;
    w.fonts[kDroid] = {true, true, true};
    FontSelection s = holopet::selectFont({kDroid}, w.ops());
    const std::string line = holopet::formatFontLogLine(s, 22);
    for (const char* key : {"font_selected=", "font_opened=",
                            "font_ascii_glyphs=", "font_cjk_glyphs=",
                            "font_size="}) {
        CHECK(line.find(key) != std::string::npos,
              std::string("8: 日志缺字段 " + std::string(key)).c_str());
    }
    CHECK(line.find(kDroid) != std::string::npos, "8: 系统路径原样保留");
    // 用户主目录必须被替换
    const std::string home_font = "/home/pet/.fonts/MyCJK.ttf";
    FakeWorld w2;
    w2.fonts[home_font] = {true, true, true};
    FontSelection s2 = holopet::selectFont({home_font}, w2.ops());
    const std::string line2 = holopet::formatFontLogLine(s2, 22);
    CHECK(line2.find("/home/pet") == std::string::npos,
          "8: 不得记录用户主目录绝对路径");
    CHECK(line2.find("<User>") != std::string::npos, "8: 必须使用占位符");
    const std::string win_home = "C:\\Users\\dell\\fonts\\cjk.ttf";
    CHECK(holopet::sanitizeFontPathForLog(win_home).find("C:\\Users\\dell") ==
              std::string::npos,
          "8: Windows 用户目录同样替换");
    // 不可用时字段仍是 no 且路径为 none
    FakeWorld w3;
    FontSelection s3 = holopet::selectFont({"", ""}, w3.ops());
    const std::string line3 = holopet::formatFontLogLine(s3, 22);
    CHECK(line3.find("font_cjk_glyphs=no") != std::string::npos,
          "8: 不可用时 cjk_glyphs=no");
    CHECK(line3.find("font_selected=none") != std::string::npos,
          "8: 不可用时不写路径");
}

// ---- 附加: 空候选/空路径不得崩溃 ----
void test_empty_candidates() {
    FakeWorld w;
    FontSelection s = holopet::selectFont({}, w.ops());
    CHECK(!s.cjk_capable && s.reason == "font_cjk_unavailable",
          "edge: 空候选 → 不可用");
    FontSelection s2 = holopet::selectFont({"", ""}, w.ops());
    CHECK(!s2.cjk_capable && s2.attempts.empty(), "edge: 空路径被跳过");
}

}  // namespace

int main() {
    test_pick_droid_over_dejavu();
    test_pick_noto_when_droid_missing();
    test_continue_on_missing_glyph();
    test_all_without_cjk_unavailable();
    test_cjk_only_font_selected_as_backup();
    test_cjk_only_backup_replaced_by_full();
    test_ascii_and_cjk_required();
    test_close_semantics();
    test_env_font_without_glyphs_rejected();
    test_log_fields_and_privacy();
    test_empty_candidates();
    if (g_fail != 0) {
        std::printf("test_font_selector: %d 处失败\n", g_fail);
        return 1;
    }
    std::printf("test_font_selector: OK (8 组行为 + 边界)\n");
    return 0;
}
