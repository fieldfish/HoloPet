#pragma once
/**
 * holo_ui.hpp — SDL2 圆屏显示层 (V3-R1)
 *
 * R1 修复:
 *  - kFontPath 直接传给 TTF_OpenFont (const char* 无 c_str())
 *  - 三个字号字体一次性加载 (title/subtitle/hint)
 *  - 构造函数 try/catch + noexcept close() 处理半初始化异常
 *  - SDL_RenderSetLogicalSize 固定 800×800 逻辑坐标
 *  - 底部提示移入圆屏安全区 (y=680)
 *
 * 只负责: SDL/TTF 初始化、窗口、Renderer、字体、绘制、资源释放。
 * 不负责: GPIO / Encoder / 业务线程 / AI。
 */

#include "display/display_config.hpp"
#include "display/state_presenter.hpp"
#include "system/logger.hpp"

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>
#include <stdexcept>
#include <cmath>
#include <string_view>

namespace holopet {

class HoloUi {
public:
    explicit HoloUi(const DisplayConfig& cfg)
        : cfg_(cfg)
    {
        try {
            cfg_.validate();

            // ---- SDL 初始化 ----
            if (SDL_Init(SDL_INIT_VIDEO) != 0) {
                throw std::runtime_error(
                    std::string("HoloUi: SDL_Init failed: ") + SDL_GetError());
            }
            init_ok_ = true;

            // ---- TTF 初始化 ----
            if (TTF_Init() != 0) {
                throw std::runtime_error(
                    std::string("HoloUi: TTF_Init failed: ") + TTF_GetError());
            }
            ttf_ok_ = true;

            // ---- 窗口 ----
            Uint32 flags = SDL_WINDOW_SHOWN;
            if (cfg_.fullscreen) {
                flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            }
            window_ = SDL_CreateWindow(
                cfg_.title.c_str(),
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                cfg_.width, cfg_.height,
                flags);
            if (!window_) {
                throw std::runtime_error(
                    std::string("HoloUi: SDL_CreateWindow failed: ") + SDL_GetError());
            }
            window_ok_ = true;

            // ---- Renderer (优先加速, 失败回退软件渲染并记录) ----
            renderer_ = SDL_CreateRenderer(window_, -1,
                                           SDL_RENDERER_ACCELERATED);
            if (!renderer_) {
                LOG_WARN("UI") << "accelerated renderer failed: " << SDL_GetError()
                               << " — falling back to software";
                renderer_ = SDL_CreateRenderer(window_, -1,
                                               SDL_RENDERER_SOFTWARE);
            }
            if (!renderer_) {
                throw std::runtime_error(
                    std::string("HoloUi: SDL_CreateRenderer failed: ") + SDL_GetError());
            }
            renderer_ok_ = true;

            // ---- 逻辑分辨率固定 800×800 (fullscreen desktop 由 SDL 缩放) ----
            if (SDL_RenderSetLogicalSize(renderer_, cfg_.width, cfg_.height) != 0) {
                throw std::runtime_error(
                    std::string("HoloUi: SDL_RenderSetLogicalSize failed: ")
                    + SDL_GetError());
            }

            // ---- 字体: 三个字号一次性加载 ----
            title_font_ = TTF_OpenFont(kFontPath, kTitleSize);
            if (!title_font_) {
                throw std::runtime_error(
                    std::string("HoloUi: TTF_OpenFont(title) failed for ")
                    + kFontPath + ": " + TTF_GetError());
            }
            subtitle_font_ = TTF_OpenFont(kFontPath, kSubtitleSize);
            if (!subtitle_font_) {
                throw std::runtime_error(
                    std::string("HoloUi: TTF_OpenFont(subtitle) failed for ")
                    + kFontPath + ": " + TTF_GetError());
            }
            hint_font_ = TTF_OpenFont(kFontPath, kHintSize);
            if (!hint_font_) {
                throw std::runtime_error(
                    std::string("HoloUi: TTF_OpenFont(hint) failed for ")
                    + kFontPath + ": " + TTF_GetError());
            }

            LOG_INFO("UI") << "initialized " << cfg_.width << "x" << cfg_.height
                           << (cfg_.fullscreen ? " (fullscreen)" : " (window)");
        }
        catch (...) {
            close();   // 清理已创建资源, 不依赖本对象析构
            throw;
        }
    }

    ~HoloUi() { close(); }

    // 禁止 copy / move (持有 SDL 裸资源)
    HoloUi(const HoloUi&) = delete;
    HoloUi& operator=(const HoloUi&) = delete;
    HoloUi(HoloUi&&) = delete;
    HoloUi& operator=(HoloUi&&) = delete;

    /** 绘制一帧状态页面 */
    void render(const StateViewModel& vm,
                std::string_view hint = "1-5: state    Esc/Q: quit") {
        int w = cfg_.width;
        int h = cfg_.height;

        // ---- 黑背景 ----
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);

        // ---- 中央状态图形: 实心圆 (半径 180) ----
        int cx = w / 2, cy = h / 2;
        SDL_SetRenderDrawColor(renderer_, 40, 90, 160, 255);
        for (int y = cy - 180; y <= cy + 180; ++y) {
            int dy = y - cy;
            int half = static_cast<int>(std::sqrt(180.0 * 180.0 - dy * dy));
            SDL_RenderDrawLine(renderer_, cx - half, y, cx + half, y);
        }

        // ---- 主标题 ----
        renderCenteredText(vm.title, title_font_, {255, 255, 255, 255},
                           h / 2 + 30);

        // ---- 副标题 ----
        renderCenteredText(vm.subtitle, subtitle_font_, {200, 200, 200, 255},
                           h / 2 + 100);

        // ---- 底部提示 (安全区内 y=680) ----
        std::string hint_str(hint);
        renderCenteredText(hint_str, hint_font_,
                           {120, 120, 120, 255}, kHintY);

        SDL_RenderPresent(renderer_);
    }

private:
    static constexpr const char* kFontPath =
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    static constexpr int kTitleSize    = 52;
    static constexpr int kSubtitleSize = 32;
    static constexpr int kHintSize     = 22;
    static constexpr int kHintY        = 680;   // 圆屏安全区内

    DisplayConfig cfg_;

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font*     title_font_    = nullptr;
    TTF_Font*     subtitle_font_ = nullptr;
    TTF_Font*     hint_font_     = nullptr;

    bool init_ok_     = false;
    bool ttf_ok_      = false;
    bool window_ok_   = false;
    bool renderer_ok_ = false;

    /** 清理所有已创建资源 (noexcept, 可处理半初始化状态) */
    void close() noexcept {
        if (title_font_)    TTF_CloseFont(title_font_);
        if (subtitle_font_) TTF_CloseFont(subtitle_font_);
        if (hint_font_)     TTF_CloseFont(hint_font_);
        title_font_ = subtitle_font_ = hint_font_ = nullptr;

        if (renderer_ok_ && renderer_) SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr; renderer_ok_ = false;

        if (window_ok_ && window_) SDL_DestroyWindow(window_);
        window_ = nullptr; window_ok_ = false;

        if (ttf_ok_) TTF_Quit();
        ttf_ok_ = false;

        if (init_ok_) SDL_Quit();
        init_ok_ = false;
    }

    void renderCenteredText(const std::string& text, TTF_Font* font,
                            const SDL_Color& color, int y) {
        if (text.empty() || !font) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surface) {
            LOG_WARN("UI") << "TTF_RenderUTF8_Blended failed: " << TTF_GetError();
            return;
        }

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);
        if (!texture) {
            LOG_WARN("UI") << "SDL_CreateTextureFromSurface failed: "
                           << SDL_GetError();
            return;
        }

        int tw = 0, th = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {
            cfg_.width / 2 - tw / 2, y - th / 2, tw, th
        };
        SDL_RenderCopy(renderer_, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
};

} // namespace holopet
