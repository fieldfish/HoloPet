/**
 * main_ai.cpp — HoloPet V6 AI 主程序 (BUILD_AI=ON)  V6-R2
 *
 * R2 强制架构:
 *  SDL 主线程: 读 InputEventQueue、drain AgentEventQueue、渲染 controller 快照;
 *               只向 AiCommandQueue 投递 typed AiCommand, 绝不直接调用 session。
 *  IO owner 线程: 唯一拥有 AiWorkerSession (socket/重连/握手/发送缓冲/
 *                 request filter); 事件进 WorkerEventQueue。
 *  退出顺序: 停止接收命令 → 请求取消 → 关 socket → join IO → 停 Encoder → 销毁 SDL。
 *
 * EC11 (BUILD_HARDWARE=ON): EncoderDevice(input_queue, cfg, GpioConfig::forPi5())
 *   默认 Pi 5 profile (A=27/B=17/SW=22, pinctrl-rp1 自动发现); 启动/停止/join 全退出路径。
 * 状态闭环: 短按只发 StartTurn/StopRecording 命令; 状态跟随 worker 事件,
 *           主程序不自行 markThinking。
 * 投影: --profile-file 加载严格 JSON; 渲染经 presentFrame 像素管线;
 *       --calibrate 界面 S 保存 (下次运行复现)。
 */

#include "agent/agent_client.hpp"
#include "audio/audio_config.hpp"
#include "agent/conversation_controller.hpp"
#include "core/app_controller.hpp"
#include "core/app_state.hpp"
#include "display/expression_renderer.hpp"
#include "display/expression_animation.hpp"
#include "features/local_feature_service.hpp"
#include "features/tool_bridge.hpp"
#include "ui/menu_controller.hpp"
#include "ui/square_menu_renderer.hpp"
#include "display/font_selector.hpp"
#include "display/text_pager.hpp"
#include "display/projection_profile.hpp"
#include "display/text_wrap.hpp"

#include <ctime>   // R8_R4_R3_R1: 时钟模式时间渲染
#include "input/input_event.hpp"
#include "input/input_queue.hpp"
#include "ipc/ai_command_queue.hpp"
#include "ipc/ai_io_loop.hpp"
#include "ipc/exit_guard.hpp"
#include "ipc/text_once.hpp"
#include "ipc/worker_args.hpp"
#include "system/logger.hpp"

#ifdef HOLOPET_HAS_HARDWARE
#include "input/encoder_device.hpp"
#include "hardware/gpio_config.hpp"
#include <csignal>
#endif

#include <SDL.h>
#include <SDL_ttf.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace holopet;

namespace {

#ifdef HOLOPET_HAS_HARDWARE
std::atomic<bool> g_running{true};
void signalHandler(int) { g_running = false; }
#endif

int64_t nowMs() {
    return static_cast<int64_t>(SDL_GetTicks64());
}


// R5_R4 (G): 工具名 → 屏幕简短动作 (不显示 JSON/内部短码)
std::string toolActionLabel(const std::string& name) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"create_timer", "正在设置定时器"}, {"list_timers", "正在查看定时器"},
        {"cancel_timer", "正在取消定时器"}, {"create_alarm", "正在设置闹钟"},
        {"list_alarms", "正在查看闹钟"},   {"enable_alarm", "正在更新闹钟"},
        {"delete_alarm", "正在删除闹钟"}, {"show_clock", "正在切换时钟"},
        {"show_pet", "正在返回宠物"},      {"add_note", "正在记便签"},
        {"list_notes", "正在查看便签"},    {"read_note", "正在读取便签"},
        {"delete_note", "正在删除便签"},   {"get_ai_mode", "正在查询模式"},
        {"set_ai_mode", "正在切换 AI 模式"}, {"dismiss_alert", "正在停止响铃"},
        {"remember_preference", "正在记住偏好"},
        {"list_preferences", "正在查看偏好"},
        {"forget_preference", "正在删除偏好"}, {"get_time", "正在查看时间"},
        {"get_status", "正在查看状态"},     {"set_volume", "正在设置音量"},
        {"set_expression", "正在调整表情"},
    };
    for (const auto& kv : kMap)
        if (name == kv.first) return kv.second;
    return "正在处理";
}

void pushKeyboardEvent(SDL_Keycode key, bool repeat, InputEventQueue& queue) {
    switch (key) {
        case SDLK_RIGHT:
        case SDLK_DOWN:  queue.push(EncoderEvent::Clockwise);        break;
        case SDLK_LEFT:
        case SDLK_UP:    queue.push(EncoderEvent::CounterClockwise); break;
        case SDLK_RETURN:
        case SDLK_SPACE:
            if (!repeat) queue.push(EncoderEvent::Pressed);
            break;
        case SDLK_l:
            if (!repeat) queue.push(EncoderEvent::LongPressed);
            break;
        default: break;
    }
}

/** R8_R4_R3_R4_R4: 定时器/闹钟到时响铃 — SDL 音频合成"叮咚"双音
 * (880→660Hz, 0.9s, 淡入淡出), 无音频文件依赖。默认设备经 launch 脚本
 * SDL_AUDIODEVICE 指向 USB 声卡; 失败静默 (视觉横幅仍在)。 */
void playFireBeep() {
    static SDL_AudioDeviceID dev = 0;
    if (dev == 0) {
        SDL_AudioSpec want{};
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (dev == 0) return;
    }
    const int sr = 44100;
    const int total = static_cast<int>(0.9 * sr);
    static std::vector<int16_t> buf;
    buf.assign(static_cast<size_t>(total), 0);
    for (int i = 0; i < total; ++i) {
        const double t = i / static_cast<double>(sr);
        const double f = t < 0.45 ? 880.0 : 660.0;
        double env = 1.0;
        if (t < 0.03) env = t / 0.03;
        const double tail = 0.9 - t;
        if (tail < 0.08) env = tail / 0.08;
        const double v = 0.35 * env * std::sin(2.0 * 3.14159265358979323846
                                                * f * t);
        buf[static_cast<size_t>(i)] = static_cast<int16_t>(v * 32767.0);
    }
    SDL_ClearQueuedAudio(dev);
    SDL_QueueAudio(dev, buf.data(),
                   static_cast<Uint32>(buf.size() * sizeof(int16_t)));
    SDL_PauseAudioDevice(dev, 0);
}

/** 严格 profile 文件加载 (缺文件 → 默认 identity + 日志) */
ProjectionProfile loadProfileFile(const std::string& path) {
    if (path.empty()) return ProjectionProfile::identity();
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("Profile") << "profile file missing, using identity: " << path;
        return ProjectionProfile::identity();
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    ProtocolError err;
    auto p = loadProjectionProfile(text, &err);
    if (!p) {
        LOG_WARN("Profile") << "profile rejected (" << err.reason
                            << "), using identity";
        return ProjectionProfile::identity();
    }
    LOG_INFO("Profile") << "loaded " << path << " scale=" << p->scale;
    return *p;
}

void saveProfileFile(const std::string& path, const ProjectionProfile& p) {
    std::ofstream f(path);
    if (!f) {
        LOG_ERROR("Profile") << "cannot write " << path;
        return;
    }
    f << serializeProjectionProfile(p);
    LOG_INFO("Profile") << "saved " << path;
}

/** R8_R2 (E): 回答分页渲染 — 每页最多 3 行 + 页码 i/n,
 *  全部居中于圆屏中部安全区 (不越窗口边缘)。 */
// R8_R3_R1 (用户要求): 数字/字母在 Pi 上显示为白色方块 —— 圆屏唯一中文字体
// (DroidSansFallbackFull) 没有 ASCII 映射。这里按**字形分流**: 中文交给 CJK
// 字体、ASCII/数字交给含 ASCII 的字体, 逐 run 渲染并累计宽度居中。
TTF_Font* asciiFontForRender() {
    static TTF_Font* cached = nullptr;
    static bool tried = false;
    if (tried) return cached;
    tried = true;
    static const char* kCandidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simsun.ttc",
    };
    for (const char* path : kCandidates) {
        TTF_Font* f = TTF_OpenFont(path, 22);
        if (!f) continue;
        const bool ok = TTF_GlyphIsProvided(f, 0x41) &&
                        TTF_GlyphIsProvided(f, 0x30) &&
                        TTF_GlyphIsProvided(f, 0x2F);   // 'A' '0' '/'
        if (ok) {
            cached = f;
            LOG_INFO("Main") << "ascii_font=" << path
                             << " (数字/字母分流渲染, 避免白色方块)";
            return cached;
        }
        TTF_CloseFont(f);
    }
    return nullptr;    // 无 ASCII 字体 → 退回单字体渲染 (不崩溃)
}

// 以 center_x 居中渲染一行; ASCII run 用 ascii 字体, 其余用 cjk 字体。
void drawTextRunsCentered(SDL_Renderer* r, TTF_Font* cjk, const std::string& text,
                          int center_x, int y) {
    if (text.empty() || !cjk) return;
    TTF_Font* ascii = asciiFontForRender();
    if (!ascii) {   // 单字体路径 (与旧行为一致)
        SDL_Surface* surf = TTF_RenderUTF8_Blended(cjk, text.c_str(),
                                                   {255, 255, 255, 255});
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_Rect dst{center_x - surf->w / 2, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
        return;
    }
    std::vector<std::pair<std::string, bool>> runs;   // (run, is_ascii)
    std::string cur;
    bool cur_ascii = false, first = true;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        const bool is_ascii = (c < 0x80);
        if (first) { cur_ascii = is_ascii; first = false; }
        else if (is_ascii != cur_ascii) {
            runs.emplace_back(cur, cur_ascii);
            cur.clear();
            cur_ascii = is_ascii;
        }
        cur.append(text, i, len);
        i += len;
    }
    if (!cur.empty()) runs.emplace_back(cur, cur_ascii);
    int total = 0;
    std::vector<int> widths;
    for (const auto& rn : runs) {
        int w = 0, h = 0;
        TTF_SizeUTF8(rn.second ? ascii : cjk, rn.first.c_str(), &w, &h);
        widths.push_back(w);
        total += w;
    }
    int x = center_x - total / 2;
    for (size_t k = 0; k < runs.size(); ++k) {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(runs[k].second ? ascii : cjk,
                                                   runs[k].first.c_str(),
                                                   {255, 255, 255, 255});
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            SDL_Rect dst{x, y, surf->w, surf->h};
            SDL_RenderCopy(r, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
            SDL_FreeSurface(surf);
        }
        x += widths[k];
    }
}

void renderAnswerPage(SDL_Renderer* r, TTF_Font* font, int size,
                      const TextPage& page) {
    if (!font) return;
    auto draw_center = [&](const std::string& text, int y) {
        // R8_R3_R1: 分流渲染 → 页脚页码 "1/2" 等数字不再显示为白色方块
        drawTextRunsCentered(r, font, text, size / 2, y);
    };
    const size_t n = page.lines.size();
    const int line_h = 34;
    const int block_h = static_cast<int>(n) * line_h;
    int y = size / 2 - block_h / 2;
    for (const auto& ln : page.lines) {
        draw_center(ln, y);
        y += line_h;
    }
    if (page.count > 1) {
        draw_center(std::to_string(page.index + 1) + "/" +
                    std::to_string(page.count), size / 2 + block_h / 2 + 12);
    }
}

void renderStatusText(SDL_Renderer* r, TTF_Font* font, int size,
                      const std::string& line1, const std::string& line2) {
    if (!font) return;
    drawTextRunsCentered(r, font, line1, size / 2, size - 48);
    drawTextRunsCentered(r, font, line2, size / 2, 8);
}

/**
 * R3-R1 (缺陷 D2): ExitGuard 抽至 ipc/exit_guard.hpp (可测试)。
 * main 在此注入 SDL 专用收尾: presentFrameDestroy → renderer →
 * window → font → TTF → SDL。
 */
holopet::ExitGuard makeSdlExitGuard(AiCommandQueue& commands,
                                    std::thread& io_thread,
                                    TTF_Font* font,
                                    SDL_Renderer* renderer,
                                    SDL_Window* window) {
    holopet::ExitGuard g;
    g.commands = &commands;
    g.io_thread = &io_thread;
    g.destroy_display = [font, renderer, window]() {
        presentFrameDestroy();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (font) TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
    };
    return g;
}

} // namespace

int main(int argc, char* argv[]) {
    bool fullscreen = false;
    bool demo_states = false;
    bool emotion_demo = false;      // R5: --emotion-demo 九表情演示
    bool calibrate = false;
    bool audio_bench = false;
    std::string profile_file;
    // R8_R2 (D)/R8_R2_R1 (A+B): --text-once-file 文字单轮入口 —
    // 显式状态机驱动 (禁止"提交后恰好 Idle"式提前成功)
    std::string text_once_file;
    std::string text_once_text;
    bool text_once_mode_done = false;
    int  text_once_rc = 0;
    int64_t page_ms = 4000;          // --text-page-ms (每页停留)
    int64_t final_hold_ms = 4000;    // --text-final-hold-ms (末页额外保持)
    std::string event_log_path;      // --text-event-log (审计 JSONL)
    std::string voice_event_log_path;  // --voice-event-log (R8_R3_R3 生产语音审计)
    bool tt_active = false;          // = !text_once_file.empty() (解析后置位)
    TextTurnLifecycle tt_life;
    TextEventAudit   tt_audit;
    bool tt_audit_opened = false;
    bool tt_submitted = false;
    std::string tt_rid;
    std::vector<std::string> tt_audit_lines;
    std::vector<TextPage> tt_pages;  // 当前回答分页缓存
    std::string tt_answer_sig;
    std::string tt_run_id;
    int tt_obs_seq = 0;
    std::string tt_content_acc;   // R8_R2_R2 (P1-5): content 累计 (SHA 对照)
    int tt_tts_started = 0;       // R8_R2_R3 (P0-3): text-only 必须可观察并判零
    int tt_tts_level = 0;
    int tt_tts_finished = 0;
    AiWorkerConfig worker_cfg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fullscreen") fullscreen = true;
        else if (a == "--demo-states") demo_states = true;
        else if (a == "--emotion-demo") emotion_demo = true;
        else if (a == "--calibrate") calibrate = true;
        else if (a == "--audio-bench") audio_bench = true;
        else if (a == "--profile-file" && i + 1 < argc) profile_file = argv[++i];
        else if (a == "--text-once-file" && i + 1 < argc) text_once_file = argv[++i];
        else if (a == "--text-page-ms" && i + 1 < argc) page_ms = std::atoll(argv[++i]);
        else if (a == "--text-final-hold-ms" && i + 1 < argc)
            final_hold_ms = std::atoll(argv[++i]);
        else if (a == "--text-event-log" && i + 1 < argc)
            event_log_path = argv[++i];
        else if (a == "--voice-event-log" && i + 1 < argc)
            voice_event_log_path = argv[++i];
        // --worker-* 由 parseWorkerArgs 统一解析 (R4-R2 F1: 三标志拆分)
    }

    // R8_R2 (D): 文字输入校验在 SDL 初始化之前 — 失败清晰 FAIL (exit 2),
    // 不弹窗口; 只记字节数, 不把全文写日志
    if (!text_once_file.empty()) {
        std::string terr;
        auto loaded = loadTextOnceFile(text_once_file, terr);
        if (!loaded) {
            LOG_ERROR("Main") << "--text-once-file rejected: " << terr
                              << " (" << text_once_file << ")";
            return 2;
        }
        text_once_text = *loaded;
        LOG_INFO("Main") << "text-once-file accepted bytes="
                         << text_once_text.size() << " (content not logged)";
        tt_active = true;
        if (page_ms < 2500 && page_ms > 0) {
            LOG_WARN("Main") << "--text-page-ms=" << page_ms
                             << " 低于建议下限 2500ms (自动测试用)";
        }
    }

    // R4-R2 (F1): 拆 uds_given/host_given/port_given 三标志 —
    // 仅 UDS + 显式 host/port 同时出现才冲突; UDS only 必须进入 UDS 配置。
    // (解析逻辑在 ipc/worker_args.hpp, 单测门 test_worker_args)
    ParsedWorkerArgs wargs = parseWorkerArgs(argc, argv);
    if (!wargs.error.empty()) {
        LOG_ERROR("Main") << wargs.error;
        return 1;
    }
    wargs.applyTo(worker_cfg);

    // R4-R2 (F1): 平台不支持显式 UDS → 清晰错误, 不静默转 TCP
#ifdef _WIN32
    if (!worker_cfg.uds_path.empty()) {
        LOG_ERROR("Main") << "Unix Domain Socket transport not supported on this platform ("
                          << "Windows 开发请用 --worker-host/--worker-port "
                          << "loopback TCP); Linux 生产使用 --worker-uds";
        return 1;
    }
#endif
    LOG_INFO("Main") << (worker_cfg.uds_path.empty()
        ? std::string("transport: loopback TCP")
        : std::string("transport: Unix Domain Socket ") + worker_cfg.uds_path);

    try {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            LOG_ERROR("Main") << "SDL_Init failed: " << SDL_GetError();
            return 1;
        }
        if (TTF_Init() != 0) {
            LOG_WARN("Main") << "TTF_Init failed (文字功能关闭): " << TTF_GetError();
        }

        const int kSize = 800;
        SDL_Window* win = SDL_CreateWindow(
            "HoloPet V6 AI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            kSize, kSize,
            SDL_WINDOW_SHOWN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!win) {
            LOG_ERROR("Main") << "window failed: " << SDL_GetError();
            return 1;
        }
        SDL_Renderer* renderer = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer) {
            LOG_ERROR("Main") << "renderer failed: " << SDL_GetError();
            return 1;
        }
        // R8_R2_R7: CJK 字形选择 (SDL2_ttf)。仅"文件可打开"不能证明含中文字形
        // (R6 Pi 实测: DejaVuSans 可开但无 你/中/文 → 白色方块)。逐个候选打开后
        // 用 TTF_GlyphIsProvided 检查规定字形, 只选用 ASCII+CJK 全部满足者;
        // 全部不合格 → font_cjk_unavailable 且 fail closed, 不得以方块宣称可读。
        std::vector<std::string> font_candidates;
        if (const char* env_font = std::getenv("HOLOPET_FONT_FILE")) {
            if (*env_font != '\0') font_candidates.push_back(env_font);
        }
        font_candidates.push_back("DejaVuSans.ttf");
        font_candidates.push_back("HoloPetCJK.ttf");
        font_candidates.push_back("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf");
        font_candidates.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
        font_candidates.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
#ifdef _WIN32
        font_candidates.push_back("C:/Windows/Fonts/msyh.ttc");
        font_candidates.push_back("C:/Windows/Fonts/simsun.ttc");
        font_candidates.push_back("C:/Windows/Fonts/simhei.ttf");
#endif
        font_candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

        holopet::FontOps font_ops;
        font_ops.probe = [](const std::string& p) {
            holopet::FontProbeResult r;
            TTF_Font* f = TTF_OpenFont(p.c_str(), 22);
            if (!f) return r;
            r.handle = f;
            r.opened = true;
            r.ascii_ok = true;
            for (unsigned cp : holopet::requiredAsciiGlyphs()) {
                if (!TTF_GlyphIsProvided(f, static_cast<Uint16>(cp))) {
                    r.ascii_ok = false;
                    break;
                }
            }
            r.cjk_ok = true;
            for (unsigned cp : holopet::requiredCjkGlyphs()) {
                if (!TTF_GlyphIsProvided(f, static_cast<Uint16>(cp))) {
                    r.cjk_ok = false;
                    break;
                }
            }
            return r;
        };
        font_ops.close = [](void* h) {
            if (h) TTF_CloseFont(static_cast<TTF_Font*>(h));
        };
        holopet::FontSelection font_sel =
            holopet::selectFont(font_candidates, font_ops);
        LOG_INFO("Main") << holopet::formatFontLogLine(font_sel, 22)
                         << " font_attempts=" << font_sel.attempts.size();
        if (font_sel.status == "font_cjk_unavailable") {
            LOG_ERROR("Main") << "font_cjk_unavailable: 无候选字体具备规定中文字形"
                                 " (你/中/文); 可设 HOLOPET_FONT_FILE 指定含中文字形的字体";
            return 2;
        }
        TTF_Font* font = static_cast<TTF_Font*>(font_sel.handle);

        ProjectionProfile profile = loadProfileFile(profile_file);

        // ---- 校准模式 (独立, 无 worker; S 保存 profile) ----
        // V6-R2: HOLOPET_PROJECTION=OFF 时不编译校准实现 (宏真语义)
        if (calibrate) {
#ifdef HOLOPET_PROJECTION
            int pattern = 0;
            bool running = true;
            LOG_INFO("Calib") << "1-5 pattern F/G flip R rotate +/- scale arrows offset"
                              << " B/b brightness G/g gamma K/k black M/m margin S save Q quit";
            while (running) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) { running = false; }
                    if (ev.type == SDL_KEYDOWN) {
                        switch (ev.key.keysym.sym) {
                            case SDLK_q: case SDLK_ESCAPE: running = false; break;
                            case SDLK_f: profile.flip_h = !profile.flip_h; break;
                            case SDLK_v: profile.flip_v = !profile.flip_v; break;
                            case SDLK_r: profile.rotate_deg = (profile.rotate_deg + 90) % 360; break;
                            case SDLK_KP_PLUS: case SDLK_EQUALS: profile.scale += 0.05; break;
                            case SDLK_KP_MINUS: case SDLK_MINUS: profile.scale -= 0.05; break;
                            case SDLK_UP:    profile.offset_y -= 4; break;
                            case SDLK_DOWN:  profile.offset_y += 4; break;
                            case SDLK_LEFT:  profile.offset_x -= 4; break;
                            case SDLK_RIGHT: profile.offset_x += 4; break;
                            case SDLK_b:     profile.brightness += 0.05; break;
                            case SDLK_n:     profile.brightness -= 0.05; break;
                            case SDLK_g:     profile.gamma += 0.05; break;
                            case SDLK_h:     profile.gamma -= 0.05; break;
                            case SDLK_k:     profile.black_level += 2; break;
                            case SDLK_j:     profile.black_level -= 2; break;
                            case SDLK_m:     profile.safe_margin += 4; break;
                            case SDLK_s:
                                saveProfileFile(profile_file.empty()
                                                ? "holopet_projection.json" : profile_file,
                                                profile);
                                break;
                            case SDLK_1: case SDLK_2: case SDLK_3:
                            case SDLK_4: case SDLK_5:
                                pattern = ev.key.keysym.sym - SDLK_1; break;
                            default: break;
                        }
                        if (!profile.valid()) {
                            LOG_WARN("Calib") << "profile out of range, reset to identity";
                            profile = ProjectionProfile::identity();
                        }
                    }
                }
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                renderCalibrationPattern(renderer, kSize, profile, pattern);
                presentFrame(renderer, kSize, kSize, profile);
                SDL_Delay(16);
            }
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(win);
            if (font) TTF_CloseFont(font);
            TTF_Quit();
            SDL_Quit();
            return 0;
#else
            LOG_ERROR("Main") << "--calibrate requires BUILD_PROJECTION=ON";
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(win);
            if (font) TTF_CloseFont(font);
            TTF_Quit();
            SDL_Quit();
            return 1;
#endif
        }

        // ---- Audio bench 入口 (HOLOPET_AUDIO 真语义) ----
#ifdef HOLOPET_AUDIO
        if (audio_bench) {
            UsbAudioConfig ac;
            LOG_INFO("Audio") << "UsbAudioConfig valid=" << (ac.valid() ? "yes" : "no")
                              << " sample_rate=" << ac.sample_rate
                              << " record_ms=" << ac.record_ms;
            LOG_INFO("Audio") << "run: ./scripts/audio_bench.sh  (Pi 实机)";
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(win);
            if (font) TTF_CloseFont(font);
            TTF_Quit();
            SDL_Quit();
            return 0;
        }
#endif

        // ---- IO owner 线程 (唯一 session 所有者) ----
        // R3: 循环体抽至 ipc/ai_io_loop.hpp (退出语义有 test_shutdown_queue_drain 门);
        //     主线程不再接触 io_running。
        AiWorkerSession session(worker_cfg);
        AiCommandQueue commands;
        WorkerEventQueue worker_events;
        std::atomic<bool> io_running{true};
        std::thread io_thread([&]() {
            runAiIoLoop(commands, session, io_running, worker_events, nowMs);
        });

        // R4-R1 (D8): IO 线程一创建就立即建立 owner guard —
        // 之后 encoder 构造/start 及任何抛异常点都在 guard 保护内
        auto guard = makeSdlExitGuard(commands, io_thread, font, renderer, win);

        // ---- 状态机 + 输入 ----
        ConversationController::Config cc_cfg;
        ConversationController controller(cc_cfg);
        AppController demo_controller;
        InputEventQueue input_queue;
        bool running = true;

#ifdef HOLOPET_HAS_HARDWARE
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        // text-once 是无头入口，不占用 EC11 GPIO，避免与常驻进程冲突。
        if (text_once_file.empty()) {
            // 生产 Pi 5 profile (A=27/B=17/SW=22, pinctrl-rp1 自动发现)
            GpioConfig gpio_cfg = GpioConfig::forPi5();
            EncoderDeviceConfig dev_cfg;
            dev_cfg.poll_interval = std::chrono::microseconds(1000);
            EncoderDevice encoder(input_queue, dev_cfg, gpio_cfg);
            guard.stop_encoder = [&encoder]() {
                encoder.stop();
                encoder.join();
            };                        // 构造后立即绑定, start 抛异常也可收尾
            encoder.start();
            LOG_INFO("Main") << "EC11 started: A=" << gpio_cfg.encoder_a
                             << " B=" << gpio_cfg.encoder_b
                             << " SW=" << gpio_cfg.encoder_sw
                             << " (chip 由 pinctrl-rp1 自动发现)";
            LOG_INFO("Main") << "若 EC11 方向与预期相反，请用 "
                             << "invert_direction 配置修正";
        } else {
            LOG_INFO("Main") << "text-once 模式: 跳过 EC11 初始化 (无头)";
        }
#endif

        // (guard 已在线程创建后立即建立; stop_encoder 在 encoder 构造后绑定)

        LOG_INFO("Main") << "HoloPet V6 AI running"
                         << (fullscreen ? " (fullscreen)" : " (window)")
                         << (demo_states ? " [demo-states]" : " [production]")
                         << (profile_file.empty() ? "" : " [profile]");

        // ---- R8_R2_R1 (A+B): text-once 状态机 / 审计 / 收尾辅助 ----
        tt_run_id = []() {
            const char* r = std::getenv("HOLOPET_RUN_ID");
            return std::string((r && *r) ? r : "local");
        }();
        auto tt_audit_add = [&](const std::string& line) {
            if (tt_audit_opened) tt_audit_lines.push_back(line);
        };
        if (tt_active) {
            tt_life.configure(60000, 60000, page_ms, final_hold_ms);
            tt_life.setStartMs(nowMs());
            if (!event_log_path.empty()) {
                if (!tt_audit.open(event_log_path)) {
                    LOG_ERROR("Main") << "--text-event-log 打开失败 (fail closed)";
                    return 7;   // PageOrAuditFail
                }
                tt_audit_opened = true;
            }
            tt_audit_add("{\"event\":\"start\",\"run_id\":\"" + tt_run_id +
                         "\",\"input_bytes\":" +
                         std::to_string(text_once_text.size()) +
                         ",\"input_sha256\":\"" + sha256Hex(text_once_text) +
                         "\",\"page_ms\":" + std::to_string(page_ms) +
                         ",\"final_hold_ms\":" + std::to_string(final_hold_ms) +
                         "}");
        }
        auto tt_observe = [&](const AgentEvent& ev, int64_t now) {
            using T = AgentEventType;
            const char* kind = nullptr;
            size_t bytes = 0;
            std::string extra;   // 不含正文的事件摘要
            switch (ev.type) {
                case T::StateChanged:
                    kind = "state";
                    extra = ",\"state\":\"" +
                            std::string(runtimeStateName(ev.state)) + "\"";
                    break;
                case T::Transcript:
                    kind = "transcript";
                    bytes = ev.text.size();
                    // 输入摘要（字节数 + SHA-256，不记录正文）
                    extra = ",\"sha256\":\"" + sha256Hex(ev.text) + "\"";
                    break;
                case T::ContentChunk:
                    kind = "content"; bytes = ev.text.size();
                    // 与 ConversationController 相同的累计上限 (R4-R1 D1)
                    if (tt_content_acc.size() + ev.text.size() <=
                        ConversationController::kMaxAnswerBytes)
                        tt_content_acc += ev.text;
                    extra = ",\"sha256\":\"" + sha256Hex(ev.text) + "\"";
                    tt_life.onEvent(TextTurnEvent::Content, ev.text, now);
                    break;
                case T::ResponseComplete:
                    kind = "response_complete";
                    tt_life.onEvent(TextTurnEvent::ResponseComplete, "", now);
                    break;
                case T::Expression: {
                    kind = "expression";
                    std::string emo = emotionName(ev.emotion);
                    for (auto& c : emo)
                        c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                    extra = ",\"emotion\":\"" + emo + "\"";
                    tt_life.onEvent(TextTurnEvent::Expression, emo, now);
                    break; }
                case T::TtsStarted:
                    kind = "tts_started";
                    ++tt_tts_started;
                    break;
                case T::TtsLevel:
                    kind = "tts_level";
                    ++tt_tts_level;
                    break;
                case T::TtsFinished:
                    kind = "tts_finished";
                    ++tt_tts_finished;
                    break;
                case T::TurnDone:
                    kind = "done";
                    tt_life.onEvent(TextTurnEvent::TurnDone, "", now);
                    break;
                case T::TurnError:
                    kind = "error";
                    // R8_R2_R3 (§6): 错误类别进审计 (稳定短码, 不含远端正文)
                    // R8_R2_R6 (A): 公开错误码 = 协议稳定 code (不再用 text)
                    extra = ",\"error_code\":\"" + ev.error_code + "\"";
                    tt_life.onEvent(TextTurnEvent::TurnError, "", now);
                    break;
                case T::TurnCancelled:
                    kind = "cancel";
                    tt_life.onEvent(TextTurnEvent::TurnCancelled, "", now);
                    break;
                case T::WorkerLost:
                    kind = "worker_lost";
                    tt_life.onEvent(TextTurnEvent::WorkerLost, "", now);
                    break;
                default: return;   // 非轮次事件 (WorkerOnline/TurnStarted) 不写 obs
            }
            if (kind && tt_audit_opened) {
                // R8_R2_R3 (§6/§7): 唯一 schema — 每条 obs 带 run_id 与该条原始
                // WorkerMessage 的 request_id (不再读共享变量), 空值也如实写出
                // 交给判定器 FAIL。
                tt_audit_add(std::string("{\"event\":\"obs\",\"seq\":") +
                             std::to_string(++tt_obs_seq) + ",\"kind\":\"" +
                             kind + "\",\"run_id\":\"" + tt_run_id +
                             "\",\"request_id\":\"" + ev.request_id +
                             "\",\"bytes\":" + std::to_string(bytes) + extra +
                             "}");
            }
        };
        auto tt_finish = [&](TextTurnExit code) {
            const int64_t finish_now = nowMs();   // R8_R2_R2: 末页停留计时终点
            if (tt_audit_opened && !tt_audit.failed()) {
                for (const auto& ln : tt_audit_lines) {
                    if (!tt_audit.writeLine(ln)) break;
                }
                const auto& pgs = tt_life.pagesShown();
                for (size_t i = 0; i < pgs.size(); ++i) {
                    const auto& pg = pgs[i];
                    tt_audit.writeLine(
                        std::string("{\"event\":\"page_shown\",\"index\":") +
                        std::to_string(pg.index) + ",\"count\":" +
                        std::to_string(pg.count) + ",\"t_ms\":" +
                        std::to_string(pg.rendered_first_ms) +
                        ",\"first_render_ms\":" +
                        std::to_string(pg.rendered_first_ms) +
                        ",\"run_id\":\"" + tt_run_id +
                        "\",\"request_id\":\"" + tt_life.requestId() +
                        "\",\"dwell_ms\":" +
                        std::to_string(tt_life.pageDwellMs(i, finish_now)) +
                        "}");
                }
                bool ok = tt_audit.writeLine(
                    std::string("{\"event\":\"terminal\",\"run_id\":\"") +
                    tt_run_id + "\",\"request_id\":\"" + tt_rid +
                    "\",\"terminal_result\":\"" +
                    (code == TextTurnExit::Success ? "success" : "failure") +
                    "\",\"content_chunks\":" +
                    std::to_string(tt_life.contentChunks()) +
                    ",\"content_bytes\":" +
                    std::to_string(tt_life.contentBytes()) +
                    ",\"content_sha256\":\"" + sha256Hex(tt_content_acc) +
                    "\"" +
                    ",\"response_complete\":" +
                    std::to_string(tt_life.responseCompleteCount()) +
                    ",\"expression\":" +
                    std::to_string(tt_life.expressionCount()) +
                    ",\"expression_value\":\"" +
                    tt_life.expressionValue() + "\"" +
                    ",\"done\":" + std::to_string(tt_life.doneCount()) +
                    ",\"error\":" + std::to_string(tt_life.errorCount()) +
                    ",\"cancel\":" + std::to_string(tt_life.cancelCount()) +
                    ",\"committed_answer_bytes\":" +
                    std::to_string(tt_life.committedBytes()) +
                    ",\"committed_answer_sha256\":\"" +
                    tt_life.committedSha256() + "\"" +
                    ",\"page_count\":" + std::to_string(tt_life.pageCount()) +
                    ",\"page_ms\":" + std::to_string(page_ms) +
                    ",\"final_hold_ms\":" + std::to_string(final_hold_ms) +
                    ",\"tts_started\":" + std::to_string(tt_tts_started) +
                    ",\"tts_level\":" + std::to_string(tt_tts_level) +
                    ",\"tts_finished\":" + std::to_string(tt_tts_finished) +
                    ",\"stale_dropped\":" +
                    std::to_string(session.staleDropped()) +
                    ",\"process_exit_code\":" +
                    std::to_string(static_cast<int>(code)) + "}");
                ok = ok && tt_audit.commit();
                if (!ok) {
                    LOG_ERROR("Main") << "text-event-log 写入失败 (fail closed)";
                    code = TextTurnExit::PageOrAuditFail;
                }
            }
            text_once_rc = static_cast<int>(code);
            LOG_INFO("Main") << "text-once finished rc=" << text_once_rc
                             << " phase=" << static_cast<int>(tt_life.phase());
            text_once_mode_done = true;
            running = false;
        };
        (void)tt_observe; (void)tt_finish; (void)tt_audit_add;

        // R8_R3_R3: --voice-event-log — 生产语音模式审计 (与 text-once 审计隔离;
        // 每条 flush; done/error/cancel 各自产生唯一 terminal)
        struct {
            std::ofstream f;
            int64_t t0 = 0;
            int seq = 0;
            bool term = false;
            bool open(const std::string& path) {
                f.open(path, std::ios::out | std::ios::trunc);
                if (!f) return false;
                t0 = nowMs();
                return true;
            }
            bool add(const std::string& line) {
                if (!f.is_open()) return false;
                f << line << "\n";
                f.flush();
                return !f.fail();
            }
        } v_audit;
        const bool v_audit_opened = !voice_event_log_path.empty();
        if (v_audit_opened && !v_audit.open(voice_event_log_path)) {
            LOG_ERROR("Main") << "--voice-event-log 打开失败 (fail closed)";
            return 7;
        }
        auto v_observe = [&](const AgentEvent& ev, int64_t now) {
            if (!v_audit_opened || v_audit.term) return;
            using T = AgentEventType;
            const char* kind = nullptr;
            size_t bytes = 0;
            std::string extra;
            switch (ev.type) {
                case T::StateChanged:
                    kind = "state";
                    extra = ",\"state\":\"" + std::string(runtimeStateName(ev.state)) + "\"";
                    break;
                case T::Transcript:
                    kind = "transcript"; bytes = ev.text.size();
                    extra = ",\"sha256\":\"" + sha256Hex(ev.text) + "\"";
                    break;
                case T::ContentChunk:
                    kind = "content"; bytes = ev.text.size();
                    extra = ",\"sha256\":\"" + sha256Hex(ev.text) + "\"";
                    break;
                case T::ResponseComplete: kind = "response_complete"; break;
                case T::Expression:
                    kind = "expression";
                    extra = ",\"emotion\":\"" + std::string(emotionName(ev.emotion)) + "\"";
                    break;
                case T::TtsStarted: kind = "tts_started"; break;
                case T::TtsLevel: kind = "tts_level"; break;
                case T::TtsFinished: kind = "tts_finished"; break;
                case T::TurnDone: kind = "done"; break;
                case T::TurnError:
                    kind = "error";
                    extra = ",\"error_code\":\"" + ev.error_code + "\"";
                    break;
                case T::TurnCancelled: kind = "cancel"; break;
                case T::WorkerOnline:
                    if (v_audit.seq != 0) return;
                    kind = "start";
                    break;
                default: return;
            }
            const bool ok = v_audit.add(
                std::string("{\"event\":\"obs\",\"seq\":") +
                std::to_string(++v_audit.seq) + ",\"kind\":\"" + std::string(kind) +
                "\",\"request_id\":\"" + ev.request_id + "\",\"bytes\":" +
                std::to_string(bytes) + ",\"t_ms\":" +
                std::to_string(now - v_audit.t0) + extra + "}");
            if (!ok) {
                LOG_ERROR("Main") << "voice-event-log 写入失败 (fail closed)";
                v_audit.term = true;
            }
            if (std::string(kind) == "done" || std::string(kind) == "error" ||
                std::string(kind) == "cancel") {
                // R8_R3_R3_R2 (必修 C §5.1): page_shown 绑定真实渲染 —
                // pagesShown 只含 markPageRendered 实际执行过的页 (未真实
                // 显示的 content 绝不产生 page_shown); dwell 以本轮终态
                // 时刻为终点。terminal 不再由 C++ 写 — 由 agentd 单点写入
                // (§5.2 每轮唯一 terminal, 含 provider 字段)。
                const int64_t end_now = nowMs();
                const auto& pgs = tt_life.pagesShown();
                for (size_t pi = 0; pi < pgs.size(); ++pi) {
                    const auto& pg = pgs[pi];
                    const int64_t dwell = tt_life.pageDwellMs(pi, end_now);
                    const int64_t shown = (pg.rendered_first_ms >= v_audit.t0)
                        ? (pg.rendered_first_ms - v_audit.t0) : 0;
                    v_audit.add(std::string(
                        "{\"type\":\"obs\",\"event\":\"page_shown\","
                        "\"request_id\":\"") +
                        ev.request_id + "\",\"page_index\":" +
                        std::to_string(pg.index) + ",\"page_count\":" +
                        std::to_string(pg.count) +
                        ",\"shown_monotonic_ms\":" + std::to_string(shown) +
                        ",\"dwell_ms\":" + std::to_string(dwell) + "}");
                }
                v_audit.term = true;
                v_audit.f.close();
            }
        };

        // R8_R4: 表情微动画控制器 (确定性; 替代旧眨眼布尔逻辑)
        ExpressionAnimationController face_anim;
        MenuLayer last_menu_layer = MenuLayer::Inactive;   // R3_R1 列表同步
        int64_t expression_until = 0;   // R5: 受控表情瞬态到期时刻
        std::string fire_banner;                 // R3_R2 到时提醒
        int64_t fire_banner_until = 0;
        RuntimeState face_state_prev = RuntimeState::Idle;
        bool content_started_notified = false;
        // R8_R4: 统一本地功能服务 + 工具桥 + 旋钮菜单 (树外数据目录)
        const std::string data_dir = [&]() {
            const char* e = std::getenv("HOLOPET_DATA_DIR");
            return e ? std::string(e) : std::string("holopet_r4_data.json");
        }();
        LocalFeatureService features(data_dir);
        features.load();
        ToolBridge tool_bridge(features);
        MenuController menu;

        while (running) {
            // 0. 连接状态快照: 每帧只取一次 (R3: 避免重复加锁/同帧显示不一致)
            const bool worker_connected = session.status().connected;

            // 1. SDL 输入
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { running = false; break; }
                if (ev.type == SDL_KEYDOWN) {
                    auto key = ev.key.keysym.sym;
                    if (key == SDLK_ESCAPE || key == SDLK_q) { running = false; break; }
                    pushKeyboardEvent(key, ev.key.repeat != 0, input_queue);
                }
            }
#ifdef HOLOPET_HAS_HARDWARE
            if (!g_running.load()) running = false;
#endif

            // 2. 输入 → 状态机 → 只投递 typed 命令 (不直接碰 session)
            EncoderEvent in_ev;
            while (input_queue.tryPop(in_ev)) {
                if (demo_states) {
                    demo_controller.handleEvent(in_ev);
                } else {
                    // R8_R4: 旋钮菜单层 (与语音共用 LocalFeatureService)
                    {
                        const bool busy =
                            controller.state() == RuntimeState::Listening ||
                            controller.state() == RuntimeState::Thinking ||
                            controller.state() == RuntimeState::Speaking;
                        MenuAction ma = menu.handle(in_ev, busy, nowMs());
                        switch (ma) {
                            case MenuAction::StartRecording:
                                commands.push(AiCommand::StartTurn);
                                break;
                            case MenuAction::StopRecording:
                                commands.push(AiCommand::StopRecording);
                                break;
                            case MenuAction::EmergencyCancel:
                                commands.push(AiCommand::CancelTurn);
                                break;
                            case MenuAction::ShowClock:
                                features.setDisplayMode(DisplayMode::Clock);
                                features.save();
                                break;
                            case MenuAction::ShowPet:
                                features.setDisplayMode(DisplayMode::Pet);
                                features.save();
                                break;
                            case MenuAction::CreateTimer:
                                features.createTimer(
                                    static_cast<int64_t>(menu.snapshot()
                                        .edit_minutes) * 60000,
                                    "", nowMs(), nowMs());
                                features.save();
                                fire_banner = "定时器已创建";
                                fire_banner_until = nowMs() + 5000;
                                break;
                            case MenuAction::CreateAlarm: {
                                auto s = menu.snapshot();
                                features.createAlarm(s.edit_hour,
                                                     s.edit_minute,
                                                     s.edit_repeat, "",
                                                     nowMs());
                                features.save();
                                fire_banner = "闹钟已设置";
                                fire_banner_until = nowMs() + 5000;
                                break;
                            }
                            case MenuAction::CancelTimer: {
                                const int ai = menu.snapshot().action_index;
                                if (ai >= 0) {
                                    features.cancelTimer(
                                        "t" + std::to_string(ai + 1));
                                    features.save();
                                    fire_banner = "定时器已取消";
                                    fire_banner_until = nowMs() + 5000;
                                }
                                break;
                            }
                            case MenuAction::DeleteAlarm: {
                                const int ai = menu.snapshot().action_index;
                                if (ai >= 0) {
                                    features.deleteAlarm(
                                        "a" + std::to_string(ai + 1));
                                    features.save();
                                    fire_banner = "闹钟已删除";
                                    fire_banner_until = nowMs() + 5000;
                                }
                                break;
                            }
                            case MenuAction::ToggleAlarm: {
                                const int ai = menu.snapshot().action_index;
                                const auto als = features.listAlarms();
                                if (ai >= 0
                                    && ai < static_cast<int>(als.size())) {
                                    features.enableAlarm(
                                        als[ai].id, !als[ai].enabled);
                                    features.save();
                                }
                                break;
                            }
                            case MenuAction::ReadNote: {
                                const int ai = menu.snapshot().action_index;
                                const auto ns = features.listNotes(100);
                                if (ai >= 0
                                    && ai < static_cast<int>(ns.size()))
                                    menu.setPreviewText(ns[ai].text);
                                break;
                            }
                            case MenuAction::DeleteNote: {
                                const int ai = menu.snapshot().action_index;
                                const auto ns = features.listNotes(100);
                                if (ai >= 0
                                    && ai < static_cast<int>(ns.size())) {
                                    features.deleteNote(ns[ai].id);
                                    features.save();
                                    menu.setPreviewText("");
                                    fire_banner = "便签已删除";
                                    fire_banner_until = nowMs() + 5000;
                                }
                                break;
                            }
                            case MenuAction::SetAiMode: {
                                static const char* kModes[] =
                                    {"fast", "auto", "deep", "local"};
                                const int ai = menu.snapshot().action_index;
                                if (ai >= 0 && ai <= 3) {
                                    features.setAiMode(kModes[ai]);
                                    features.save();
                                    LOG_INFO("Main")
                                        << "ai_mode set to " << kModes[ai];
                                    static const char* kModeNames[] =
                                        {"快速", "自动", "深度", "本地"};
                                    fire_banner = std::string("已切换:")
                                        + kModeNames[ai];
                                    fire_banner_until = nowMs() + 5000;
                                }
                                break;
                            }
                            case MenuAction::None:
                            default:
                                break;
                        }
                        // 菜单活跃时吞掉本应发给会话控制器的同一事件
                        if (ma != MenuAction::None
                            || menu.snapshot().active) {
                            continue;
                        }
                    }
                    auto cmd = controller.handleEncoder(in_ev, nowMs());
                    switch (cmd) {
                        case ConversationController::Command::StartTurn:
                            commands.push(AiCommand::StartTurn, "",
                                          features.aiMode());
                            break;   // 状态闭环: 保持 Listening, 等 worker 事件
                        case ConversationController::Command::StopRecording:
                            commands.push(AiCommand::StopRecording);
                            break;
                        case ConversationController::Command::StopSpeak:
                        case ConversationController::Command::CancelTurn:
                            commands.push(AiCommand::CancelTurn);
                            break;
                        case ConversationController::Command::None:
                        default:
                            break;
                    }
                }
            }

            // 3. Worker 事件 → 状态机 (主线程唯一消费者)
            //    R8_R2_R1 (A): text-once 激活时同一事件流喂给生命周期状态机
            AgentEvent w_ev;
            while (worker_events.tryPop(w_ev)) {
                if (tt_active && !text_once_mode_done)
                    tt_observe(w_ev, nowMs());
                if (v_audit_opened && !v_audit.term)
                    v_observe(w_ev, nowMs());
                if (w_ev.type == AgentEventType::TurnError
                    && w_ev.error_code == "local_llm_unavailable") {
                    fire_banner = "本地模型未就绪";
                    fire_banner_until = nowMs() + 5000;
                }
                if (w_ev.type == AgentEventType::Expression) {
                    // R8_R4_R3_R4_R5: 受控表情瞬态显示 6s, 到期恢复状态姿态
                    expression_until = nowMs() + 6000;
                }
                if (w_ev.type == AgentEventType::TurnError
                    && w_ev.error_code == "stt_offline") {
                    fire_banner = "当前离线，语音暂不可用";
                    fire_banner_until = nowMs() + 5000;
                }
                if (w_ev.type == AgentEventType::ToolRequest) {
                    LOG_INFO("Main") << "tool_request received bytes="
                                     << w_ev.text.size();
                    auto tr = parseToolRequest(w_ev.text);
                    if (!tr) {
                        LOG_INFO("Main") << "tool_request parse FAILED";
                    }
                    if (tr) {
                        ToolResult r = tool_bridge.handle(*tr, nowMs(), nowMs());
                        commands.push(AiCommand::SendRaw,
                                      dumpToolResult(tr->request_id,
                                                     tr->tool_call_id, r));
                        if (tool_bridge.featureEvent().emitted) {
                            const auto& fe = tool_bridge.featureEvent();
                            commands.push(AiCommand::SendRaw,
                                "{\"type\":\"feature_event\","
                                "\"v\":\"1\",\"feature\":\"" + fe.feature
                                + "\",\"action\":\"" + fe.action
                                + "\",\"item_id\":\"" + fe.item_id
                                + "\",\"monotonic_ms\":"
                                + std::to_string(fe.monotonic_ms) + "}");
                        }
                        features.save();
                        LOG_INFO("Main") << "tool_result sent name="
                                         << tr->name << " ok=" << r.ok
                                         << " code=" << r.code
                                         << " args_size=" << tr->args.size();
                    }
                }
                if (w_ev.type == AgentEventType::ToolCall) {
                    // R5_R4 (G): 工具执行反馈 — 屏幕显示简短动作标签
                    fire_banner = toolActionLabel(w_ev.text);
                    fire_banner_until = nowMs() + 4000;
                }
                controller.handleAgent(w_ev, nowMs());
            }
            // 离线提示: 渲染层读每帧快照, 状态机由 WorkerLost 事件驱动
            (void)worker_connected;

            // 3.5 R8_R2_R1 (A): --text-once-file — 显式状态机判定
            //     成功必须满足十条 (见 text_once.hpp); 失败分级退出码;
            //     Provider/终态等待与页面展示时间分别计算
            if (tt_active && !text_once_mode_done) {
                const int64_t now = nowMs();
                if (!tt_submitted) {
                    if (worker_connected) {
                        if (commands.push(AiCommand::SubmitTextTurn,
                                          text_once_text,
                                          features.aiMode())) {
                            tt_submitted = true;
                            LOG_INFO("Main") << "text turn submitted bytes="
                                             << text_once_text.size();
                        }
                    }
                } else if (tt_rid.empty()) {
                    // 等待 IO 线程生成真实 request_id (观测后绑定)
                    const std::string rid = session.textTurnRequestId();
                    if (!rid.empty()) {
                        tt_rid = rid;
                        tt_life.onSubmitted(rid, now);
                        tt_audit_add("{\"event\":\"submitted\",\"request_id\":\"" +
                                     tt_rid + "\"}");
                    }
                }
                if (tt_submitted) {
                    tt_life.setAnswerState(controller.answerValid(),
                                           controller.committedAnswer());
                    const int64_t to = tt_life.pollTimeout(now);
                    if (to >= 0) {
                        tt_life.finishWith(static_cast<TextTurnExit>(to));
                        tt_finish(tt_life.exitCode());
                    } else if (tt_life.errorCount() > 0 ||
                               tt_life.cancelCount() > 0) {
                        tt_life.markTerminalError();
                        tt_finish(tt_life.exitCode());
                    } else if (tt_life.readyToFinish(now)) {
                        tt_life.finishSuccess();
                        tt_finish(tt_life.exitCode());
                    }
                }
            }

            // 4. 周期
            if (!demo_states) controller.tick(nowMs());

            // 5. 表情微动画 (R8_R4): 逐帧 tick; 状态切换 setState;
            //    首个内容到达 notifyContentStarted (视觉回基准);
            //    相位审计事件低频写入 voice 审计 (§3.6)。
            {
                RuntimeState bs = controller.state();
                if (bs != face_state_prev) {
                    face_anim.setState(bs, nowMs());
                    face_state_prev = bs;
                }
                if (!content_started_notified && controller.answerValid()) {
                    face_anim.notifyContentStarted(nowMs());
                    content_started_notified = true;
                }
                if (bs != RuntimeState::Thinking
                    && !controller.answerValid()) {
                    content_started_notified = false;
                }
                // R8_R4_R3_R4_R5: 菜单/时钟页面不绘制宠物眨眼
                const bool face_paused =
                    menu.snapshot().active
                    || features.displayMode() == DisplayMode::Clock;
                FaceAnimAudit fa = face_anim.tick(nowMs(), face_paused);
                if (fa.emitted && v_audit_opened && !v_audit.term) {
                    std::string line = std::string(
                        "{\"type\":\"obs\",\"event\":\"face_anim\","
                        "\"kind\":\"") + fa.event + "\",\"request_id\":\"" +
                        tt_life.requestId() + "\",\"t_ms\":" +
                        std::to_string(nowMs() - v_audit.t0);
                    if (!fa.direction.empty())
                        line += ",\"direction\":\"" + fa.direction + "\"";
                    line += "}";
                    v_audit.add(line);
                }
            }

            // 6. 渲染 (controller 快照 + 投影像素管线)
            {
                // R8_R4_R3_R4_R5: 受控表情到期恢复状态对应姿态 (Neutral)
                Emotion render_emotion = Emotion::Neutral;
                if (emotion_demo) {
                    // 九表情确定性演示: 2s/个, 3 圈后自动恢复常驻
                    static const Emotion kDemoNine[9] = {
                        Emotion::Happy, Emotion::Surprised, Emotion::Craving,
                        Emotion::Concerned, Emotion::Speechless, Emotion::Joy,
                        Emotion::Angry, Emotion::Curious, Emotion::Cute,
                    };
                    static int64_t demo_start = -1;
                    if (demo_start < 0) demo_start = nowMs();
                    const int64_t el = nowMs() - demo_start;
                    if (el < 9 * 2000 * 3)
                        render_emotion = kDemoNine[(el / 2000) % 9];
                    else
                        emotion_demo = false;
                } else if (expression_until != 0
                           && nowMs() < expression_until) {
                    render_emotion = controller.emotion();
                }
                ExpressionParams params = computeExpression(
                    controller.state(), render_emotion,
                    controller.ttsLevel());
                renderFace(renderer, kSize / 2, kSize / 2, 340, params,
                           face_anim.frame(), nowMs());

                // R8_R4_R2 (7.3): 慢回答等待提示 — Thinking 且未出内容时
                // 显示, 首段内容/终态由 ConversationController 清除
                if (controller.state() == RuntimeState::Thinking
                    && !controller.waitingHint().empty()) {
                    drawTextRunsCentered(renderer, font,
                                         controller.waitingHint(),
                                         kSize / 2, kSize - 64);
                }
                // R8_R4_R3_R2: 定时器/闹钟到时横幅 (宠物视图)
                if (!fire_banner.empty()) {
                    drawTextRunsCentered(renderer, font, fire_banner,
                                         kSize / 2, kSize - 64);
                }

                // R8_R4_R3_R1: 生产模式不再绘制状态 HUD (§3.1 无 HUD;
                // Pi §11 实测屏幕出现 "Idle / neutral" 字样)

                // R8_R4_R3_R2: 定时器/闹钟到时提醒 (§6 到时真实提醒一次;
                // B2 FireEvent 此前未接线 — 轮询 tick 产生视觉横幅 8s)
                {
                    // R5_R4 (B2): 停止响铃 — 立即清除横幅/停止提醒
                    if (features.consumeAlertDismiss()) {
                        fire_banner.clear();
                        fire_banner_until = 0;
                    }
                    const auto fires = features.tick(nowMs(), nowMs());
                    for (const auto& fe : fires) {
                        fire_banner = (fe.kind == "timer")
                            ? "定时器到!" : "闹钟响了!";
                        fire_banner_until = nowMs() + 8000;
                        playFireBeep();   // R8_R4_R3_R4_R4: 直接响铃
                        LOG_INFO("Main") << "feature fire kind=" << fe.kind
                                         << " id=" << fe.id;
                    }
                    if (fire_banner_until != 0
                        && nowMs() > fire_banner_until) {
                        fire_banner.clear();
                        fire_banner_until = 0;
                    }
                }

                // 旋钮菜单作为覆盖层渲染。菜单活跃时覆盖回答/状态文字，
                // 退出后恢复原画面；菜单不是默认常驻界面。
                const MenuSnapshot msnap = menu.snapshot();

                // R8_R4_R3_R1: 列表层进入时同步真实数据 (便签/定时器/闹钟)
                if (msnap.active && msnap.layer != last_menu_layer) {
                    if (msnap.layer == MenuLayer::Root) {
                        // R8_R4_R3_R4_R4: 根菜单首项随显示模式动态命名
                        menu.syncItems(std::vector<std::string>{
                            features.displayMode() == DisplayMode::Clock
                                ? "切换回表情" : "切换为时钟",
                            "定时器", "闹钟", "便签", "AI模式"});
                    } else if (msnap.layer == MenuLayer::AiMode) {
                        const std::string& m = features.aiMode();
                        menu.setIndex(m == "fast" ? 0 : m == "auto" ? 1
                                     : m == "deep" ? 2 : 3);
                    } else if (msnap.layer == MenuLayer::NoteList) {
                        const auto ns = features.listNotes(100);
                        std::vector<std::string> its;
                        for (size_t i = 0; i < ns.size(); ++i) {
                            std::string t = ns[i].text;
                            if (t.size() > 16) t = t.substr(0, 16) + "...";
                            its.push_back(std::to_string(i + 1) + ". " + t);
                        }
                        menu.syncItems(its.empty()
                            ? std::vector<std::string>{"(无便签)"} : its);
                    } else if (msnap.layer == MenuLayer::TimerList) {
                        // R8_R4_R3_R4_R5_R2: 首项"新建", 其余真实定时器
                        const auto ts = features.listTimers();
                        std::vector<std::string> its{"新建定时器"};
                        for (size_t i = 0; i < ts.size(); ++i) {
                            const int64_t remain = (std::max)(int64_t{0},
                                (ts[i].ends_mono_ms - nowMs()) / 1000);
                            its.push_back(std::to_string(i + 1) + ". 剩 "
                                + std::to_string(remain) + "s");
                        }
                        menu.syncItems(its);
                    } else if (msnap.layer == MenuLayer::AlarmList) {
                        const auto as = features.listAlarms();
                        std::vector<std::string> its{"新建闹钟"};
                        for (size_t i = 0; i < as.size(); ++i) {
                            its.push_back(std::to_string(i + 1) + ". "
                                + std::to_string(as[i].hour) + ":"
                                + std::to_string(as[i].minute)
                                + (as[i].enabled ? "" : " (关)"));
                        }
                        menu.syncItems(its);
                    }
                    last_menu_layer = msnap.layer;
                }
                if (!msnap.active) last_menu_layer = MenuLayer::Inactive;

                // R8_R4_R3_R1: 时钟模式渲染 — 蓝色圆形指针钟 (§6 常亮时钟;
                // 用户要求: 与既往蓝底圆形时钟一致, 时/分/秒三针, 秒针
                // 平滑走动; 时间取系统本地时间 — Pi systemd-timesyncd 在线
                // 自动 NTP 同步, 离线走 RTC 继续运行)
                if (features.displayMode() == DisplayMode::Clock
                    && !msnap.active) {
                    const int ccx = kSize / 2, ccy = kSize / 2;
                    const int cr = 300;
                    // 表盘 (R8_R4_R3_R4_R4: 经典指针钟 — 外双环 + 主刻度 +
                    // 细分刻度 + 内环; 蓝线蓝字, 黑底)
                    SDL_SetRenderDrawColor(renderer, kFaceColor.r,
                                           kFaceColor.g, kFaceColor.b, 255);
                    renderFilledCircle(renderer, ccx, ccy, cr, 96);
                    renderFilledCircle(renderer, ccx, ccy, cr - 7, 96,
                                       kBackdrop);
                    SDL_SetRenderDrawColor(renderer, kFaceColor.r,
                                           kFaceColor.g, kFaceColor.b, 255);
                    for (int pass = 0; pass < 2; ++pass) {
                        for (int ti = 0; ti < 96; ++ti) {
                            const double ta = ti
                                * (3.14159265358979323846 / 48.0);
                            SDL_RenderDrawLineF(renderer,
                                static_cast<float>(ccx + std::cos(ta)
                                    * (cr - 7 - pass)),
                                static_cast<float>(ccy + std::sin(ta)
                                    * (cr - 7 - pass)),
                                static_cast<float>(ccx + std::cos(ta)
                                    * (cr - 24 - pass)),
                                static_cast<float>(ccy + std::sin(ta)
                                    * (cr - 24 - pass)));
                        }
                    }
                    // 内环 (分针域边界)
                    for (int ti = 0; ti < 96; ++ti) {
                        const double ta = ti
                            * (3.14159265358979323846 / 48.0);
                        SDL_RenderDrawLineF(renderer,
                            static_cast<float>(ccx + std::cos(ta) * 251),
                            static_cast<float>(ccy + std::sin(ta) * 251),
                            static_cast<float>(ccx + std::cos(ta) * 253),
                            static_cast<float>(ccy + std::sin(ta) * 253));
                    }
                    // 主刻度 (12/3/6/9 粗长, 其余 5 分中长)
                    for (int ti = 0; ti < 12; ++ti) {
                        const bool cardinal = (ti % 3 == 0);
                        const double ta = ti
                            * (3.14159265358979323846 / 6.0);
                        const double r1 = cr - (cardinal ? 36 : 31);
                        const double r2 = cr - 9;
                        const double dx = std::cos(ta), dy = std::sin(ta);
                        const double hw = cardinal ? 1.2 : 0.5;
                        for (double w = -hw; w <= hw; w += 0.5) {
                            SDL_RenderDrawLineF(renderer,
                                static_cast<float>(ccx + dx * r1 + w),
                                static_cast<float>(ccy + dy * r1 + w),
                                static_cast<float>(ccx + dx * r2 + w),
                                static_cast<float>(ccy + dy * r2 + w));
                        }
                    }
                    // 细分刻度 (每分钟)
                    for (int ti = 0; ti < 60; ++ti) {
                        if (ti % 5 == 0) continue;
                        const double ta = ti
                            * (3.14159265358979323846 / 30.0);
                        SDL_RenderDrawLineF(renderer,
                            static_cast<float>(ccx + std::cos(ta)
                                * (cr - 27)),
                            static_cast<float>(ccy + std::sin(ta)
                                * (cr - 27)),
                            static_cast<float>(ccx + std::cos(ta)
                                * (cr - 22)),
                            static_cast<float>(ccy + std::sin(ta)
                                * (cr - 22)));
                    }
                    // 时间 (本地时区; 秒针加帧内毫秒平滑)
                    std::time_t nowt = std::time(nullptr);
                    std::tm ltm{};
#ifdef _WIN32
                    localtime_s(&ltm, &nowt);
#else
                    localtime_r(&nowt, &ltm);
#endif
                    const double ms_frac = (nowMs() % 1000) / 1000.0;
                    const double sec = ltm.tm_sec + ms_frac;
                    const double min = ltm.tm_min + sec / 60.0;
                    const double hr = (ltm.tm_hour % 12) + min / 60.0;
                    auto hand = [&](double val, double steps_per_turn,
                                    double len, double tail, double width) {
                        const double a = (val / steps_per_turn * 2.0
                            * 3.14159265358979323846)
                            - 3.14159265358979323846 / 2.0;
                        const double dx = std::cos(a), dy = std::sin(a);
                        for (double w = -width / 2.0; w <= width / 2.0;
                             w += 0.5) {
                            SDL_RenderDrawLineF(renderer,
                                static_cast<float>(ccx - dx * tail + w),
                                static_cast<float>(ccy - dy * tail + w),
                                static_cast<float>(ccx + dx * len + w),
                                static_cast<float>(ccy + dy * len + w));
                        }
                    };
                    hand(hr, 12.0, cr * 0.46, cr * 0.10, 5.0);   // 时针(带尾)
                    hand(min, 60.0, cr * 0.68, cr * 0.14, 4.0);   // 分针(带尾)
                    hand(sec, 60.0, cr * 0.80, cr * 0.20, 1.5);   // 秒针(带尾)
                    // 秒针尾部配重
                    {
                        const double a = (sec / 60.0 * 2.0
                            * 3.14159265358979323846)
                            - 3.14159265358979323846 / 2.0;
                        const double dx = std::cos(a), dy = std::sin(a);
                        renderFilledCircle(renderer,
                            static_cast<int>(ccx - dx * cr * 0.20),
                            static_cast<int>(ccy - dy * cr * 0.20), 5, 16);
                    }
                    // 中心轴帽 + 轴点
                    renderFilledCircle(renderer, ccx, ccy, 8, 24);
                    SDL_SetRenderDrawColor(renderer, kBackdrop.r,
                                           kBackdrop.g, kBackdrop.b, 255);
                    renderFilledCircle(renderer, ccx, ccy, 3, 16);
                    SDL_SetRenderDrawColor(renderer, kFaceColor.r,
                                           kFaceColor.g, kFaceColor.b, 255);
                    // 日期 (次级蓝) 与退出提示
                    {
                        char dbuf[32];
                        std::snprintf(dbuf, sizeof(dbuf), "%d月%d日",
                                      ltm.tm_mon + 1, ltm.tm_mday);
                        renderSquareTextCentered(
                            renderer, font, asciiFontForRender(),
                            kSquareSecondary, dbuf, ccx, ccy + 92);
                    }
                    renderSquareTextCentered(renderer, font,
                                            asciiFontForRender(),
                                            kSquareSecondary, "长按进菜单",
                                            ccx, kSize - 52);
                    if (!fire_banner.empty()) {
                        drawTextRunsCentered(renderer, font, fire_banner,
                                             kSize / 2, kSize - 70);
                    }
                    presentFrame(renderer, kSize, kSize, profile);
                    SDL_Delay(4);
                    continue;
                }

                if (msnap.active) {
                    // R8_R4_R3_R4_R5: 方形列表菜单 (用户最终确认)
                    renderSquareMenu(renderer, kSize, msnap,
                                     font, asciiFontForRender());
                    presentFrame(renderer, kSize, kSize, profile);
                    SDL_Delay(4);
                    continue;
                }

                // R8_R2_R1 (B): 已提交回答 → 完整分页显示, 由
                // TextTurnLifecycle 统一驱动 (每页实际渲染一次后才允许翻页,
                // 末页达到 final hold 后 text-once 才可退出);
                // 回答变化/新轮/cancel/error 重新绑定分页
                const std::string answer = controller.answerValid()
                    ? controller.committedAnswer() : std::string();
                if (!answer.empty()) {
                    if (answer != tt_answer_sig) {
                        tt_answer_sig = answer;        // 新回答: 重新分页
                        tt_pages = paginateText(answer, 24, 3);
                        if (!tt_pages.empty()) {
                            tt_life.bindPages(static_cast<int>(tt_pages.size()),
                                              nowMs());
                            if (tt_audit_opened) {
                                for (const auto& pg : tt_life.pagesShown()) {
                                    tt_audit_add(
                                        std::string("{\"event\":\"page_bind\",\"index\":") +
                                        std::to_string(pg.index) +
                                        ",\"count\":" + std::to_string(pg.count) +
                                        "}");
                                }
                            }
                        }
                    }
                    if (!tt_pages.empty()) {
                        if (tt_life.shouldAdvancePage(nowMs())) {
                            tt_life.advancePage(nowMs());
                        }
                        size_t idx = 0;
                        if (!tt_life.pagesShown().empty())
                            idx = static_cast<size_t>(
                                tt_life.pagesShown().back().index);
                        if (idx >= tt_pages.size()) idx = tt_pages.size() - 1;
                        renderAnswerPage(renderer, font, kSize, tt_pages[idx]);
                        tt_life.markPageRendered(nowMs());   // 本页实际渲染
                    }
                } else {
                    tt_answer_sig.clear();
                }

                presentFrame(renderer, kSize, kSize, profile);
            }

            SDL_Delay(4);
        }

        // ---- 退出顺序 (R3): 停命令 → cancel → Shutdown → IO 消费后自退
        //      → join IO → 停 Encoder → presentFrameDestroy → 销毁 SDL。
        // 全部由 ExitGuard 统一执行 (异常路径同一段代码)。
        commands.push(AiCommand::CancelTurn);
        commands.push(AiCommand::Shutdown);
        // (io_running 不再由主线程置位 — 见 ai_io_loop.hpp)
        LOG_INFO("Main") << "goodbye.";
        // R8_R2 (D): --text-once-file 模式以该轮结果作为退出码 (0=完成, 3=超时)
        return text_once_rc;   // ExitGuard 析构: join IO → 停 Encoder → 销毁 SDL

    } catch (const std::exception& ex) {
        LOG_ERROR("Main") << "fatal: " << ex.what();
        return 1;
    }
}
