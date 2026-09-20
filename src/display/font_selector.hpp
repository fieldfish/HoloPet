#pragma once
// R8_R2_R7: CJK 字形选择策略 (SDL2_ttf 兼容, 不依赖 SDL3 fallback API)。
//
// 背景 (R6 Pi 实测根因): 仅凭 "TTF_OpenFont 成功" 不能证明字体含中文字形;
// DejaVuSans 可打开但缺 你/中/文 → 屏幕显示白色方块。正确做法是逐个候选
// 打开后用 TTF_GlyphIsProvided 检查规定字形, 只选用同时具备 ASCII 与
// 代表性中文字形的候选; 全部不合格时显式返回 font_cjk_unavailable。
//
// 本头文件不包含 SDL, 只描述策略与注入接口, 便于在无字体安装的机器上做
// 行为测试; main_ai.cpp 负责把 SDL2_ttf 的 open/glyph/close 接入 FontOps。
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace holopet {

// 规定检查字形 (均为 BMP, SDL2_ttf 的 Uint16 接口即可覆盖)
//   ASCII: 'A' '0'    CJK: 你 U+4F60, 中 U+4E2D, 文 U+6587
inline const std::vector<unsigned>& requiredAsciiGlyphs() {
    static const std::vector<unsigned> v = {0x41u, 0x30u};
    return v;
}

inline const std::vector<unsigned>& requiredCjkGlyphs() {
    static const std::vector<unsigned> v = {0x4F60u, 0x4E2Du, 0x6587u};
    return v;
}

// 单个候选的探测结果 (由 FontOps::probe 填充)
struct FontProbeResult {
    void* handle = nullptr;   // 打开成功时的字体句柄 (TTF_Font*)
    bool opened = false;      // 文件可打开
    bool ascii_ok = false;    // 全部 ASCII 规定字形存在
    bool cjk_ok = false;      // 全部 CJK 规定字形存在
};

// 调用方注入的操作 (测试可给假实现; main_ai 接入 SDL2_ttf)
struct FontOps {
    // 打开并检查字形; 不得在此关闭句柄 (句柄由选择器决定去留)
    std::function<FontProbeResult(const std::string&)> probe;
    // 关闭未被选中的句柄 (选中句柄由外层 guard 关闭一次)
    std::function<void(void*)> close;
};

// 每个候选的记录 (供日志与测试断言)
struct FontAttempt {
    std::string path;
    bool opened = false;
    bool ascii_ok = false;
    bool cjk_ok = false;
    bool kept = false;               // 被选中保留
    bool closed_unselected = false;  // 未被选中且已关闭一次
};

struct FontSelection {
    bool cjk_capable = false;   // §4.2: ASCII+CJK 全部满足才为 true
    std::string path;           // 选中候选路径 (选中成功时有效)
    void* handle = nullptr;     // 选中句柄; 由外层关闭一次
    std::string reason;         // "selected" 或 "font_cjk_unavailable"
    std::string status;         // selected_full / selected_cjk_only / font_cjk_unavailable
    std::vector<FontAttempt> attempts;
};

// 逐个探测候选:
//   - 淘汰条件只有"缺任一规定 CJK glyph"(§4.2);
//   - 优先选择 ASCII+CJK 全满足者 (selected_full, cjk_capable=yes);
//   - 若只有"仅 CJK"候选 (Pi 上 Droid 即此类: 有 你/中/文 但无 ASCII 映射),
//     则选中它并标记 selected_cjk_only (中文可读; ASCII 记录为 no,
//     §4.2 规定此时不得标记 cjk_capable);
//   - 全部候选都缺 CJK → font_cjk_unavailable (调用方必须 fail/blocked)。
// 未选中的句柄立即 close; 选中句柄保留交给外层 guard。
inline FontSelection selectFont(const std::vector<std::string>& candidates,
                                const FontOps& ops) {
    FontSelection sel;
    sel.reason = "font_cjk_unavailable";
    sel.status = "font_cjk_unavailable";
    int backup_index = -1;          // 已记住的"仅 CJK"后备候选
    void* backup_handle = nullptr;
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        FontAttempt at;
        at.path = path;
        FontProbeResult pr;
        if (ops.probe) pr = ops.probe(path);
        at.opened = pr.opened && pr.handle != nullptr;
        at.ascii_ok = at.opened && pr.ascii_ok;
        at.cjk_ok = at.opened && pr.cjk_ok;
        if (at.cjk_ok && at.ascii_ok) {
            at.kept = true;
            sel.cjk_capable = true;
            sel.path = path;
            sel.handle = pr.handle;
            sel.reason = "selected";
            sel.status = "selected_full";
            sel.attempts.push_back(at);
            if (backup_handle && ops.close) {
                ops.close(backup_handle);   // 后备已不需要
                sel.attempts[backup_index].closed_unselected = true;
            }
            return sel;
        }
        if (at.cjk_ok && backup_handle == nullptr) {
            backup_index = static_cast<int>(sel.attempts.size());
            backup_handle = pr.handle;
            sel.attempts.push_back(at);   // 暂不关闭, 继续找双满足者
            continue;
        }
        if (at.opened && ops.close) {
            ops.close(pr.handle);
            at.closed_unselected = true;
        } else if (!at.opened) {
            at.closed_unselected = true;   // 未打开: 无需关闭, 视为已清理
        }
        sel.attempts.push_back(at);
    }
    if (backup_handle) {
        sel.path = sel.attempts[backup_index].path;
        sel.handle = backup_handle;
        sel.attempts[backup_index].kept = true;
        sel.reason = "selected";
        sel.status = "selected_cjk_only";   // 中文可读, ASCII 缺失需如实记录
        sel.cjk_capable = false;
    }
    return sel;
}

// 选中路径的日志安全形式: 用户主目录 (Linux /home/<user>、Windows C:\Users\<user>)
// 替换为占位符; 系统字体路径 (/usr/share/...) 原样保留。
inline std::string sanitizeFontPathForLog(const std::string& path) {
    const std::string linux_home = "/home/";
    if (path.rfind(linux_home, 0) == 0) {
        const std::size_t slash = path.find('/', linux_home.size());
        if (slash == std::string::npos) return "/home/<User>/...";
        return "/home/<User>" + path.substr(slash);
    }
    const std::string win_home = "C:\\Users\\";
    if (path.rfind(win_home, 0) == 0) {
        const std::size_t slash = path.find('\\', win_home.size());
        if (slash == std::string::npos) return "C:\\Users\\<User>\\...";
        return "C:\\Users\\<User>" + path.substr(slash);
    }
    return path;
}

// 启动日志字段 (R8_R2_R7 §4.3): 五个字段齐全, 且不含用户主目录绝对路径。
// 字段反映实际选中字体的能力 (仅 CJK 字体: font_ascii_glyphs=no, font_cjk_glyphs=yes)。
inline std::string formatFontLogLine(const FontSelection& sel, int size) {
    bool ascii = false, cjk = false;
    for (const FontAttempt& a : sel.attempts) {
        if (a.kept) {
            ascii = a.ascii_ok;
            cjk = a.cjk_ok;
        }
    }
    const bool ok = sel.reason == "selected";
    std::string line;
    line += "font_selected=" + sanitizeFontPathForLog(ok ? sel.path
                                                         : std::string("none"));
    line += " font_opened=" + std::string(ok ? "yes" : "no");
    line += " font_ascii_glyphs=" + std::string(ascii ? "yes" : "no");
    line += " font_cjk_glyphs=" + std::string(cjk ? "yes" : "no");
    line += " font_size=" + std::to_string(size);
    line += " font_status=" + (sel.status.empty() ? sel.reason : sel.status);
    return line;
}

}  // namespace holopet
