/**
 * main_display.cpp — HoloPet V3 SDL2 圆屏显示 Demo  V3-R1
 *
 * R1 修复:
 *  - SDLK_Q 删除 (SDL2 只有 SDLK_q)
 *  - setState helper: 只有状态真的变化才 render
 *
 * 功能:
 *  - 800×800 窗口 (--fullscreen 全屏)
 *  - 键盘 1-5 切换 AppState
 *  - Esc / Q 退出
 *  - dirty render: 仅状态变化时重绘
 *
 * 不接 GPIO / EC11 / 硬件线程。
 */

#include "display/display_config.hpp"
#include "display/state_presenter.hpp"
#include "display/holo_ui.hpp"
#include "core/app_state.hpp"
#include "system/logger.hpp"

#include <SDL.h>

#include <string>

using namespace holopet;

int main(int argc, char* argv[]) {
    // ---- 解析命令行 ----
    DisplayConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--fullscreen") {
            cfg.fullscreen = true;
        }
    }

    try {
        HoloUi ui(cfg);

        AppState state = AppState::Idle;
        bool dirty = true;

        LOG_INFO("Main") << "HoloPet V3 Display Demo ("
                         << (cfg.fullscreen ? "fullscreen" : "window") << ")";

        // 只有状态真的变化才置 dirty
        auto setState = [&](AppState next) {
            if (state != next) {
                state = next;
                dirty = true;
                LOG_INFO("State") << "→ " << appStateName(state);
            }
        };

        SDL_Event ev;
        bool running = true;

        while (running) {
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    running = false;
                    break;
                }
                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_1: setState(AppState::Idle);      break;
                        case SDLK_2: setState(AppState::Listening); break;
                        case SDLK_3: setState(AppState::Thinking);  break;
                        case SDLK_4: setState(AppState::Speaking);  break;
                        case SDLK_5: setState(AppState::Error);     break;
                        case SDLK_RIGHT:
                        case SDLK_DOWN: {
                            int idx = static_cast<int>(state);
                            setState(static_cast<AppState>((idx + 1) % 5));
                            break;
                        }
                        case SDLK_LEFT:
                        case SDLK_UP: {
                            int idx = static_cast<int>(state);
                            setState(static_cast<AppState>((idx + 4) % 5));
                            break;
                        }
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            running = false;
                            break;
                        default:
                            break;
                    }
                }
            }

            // ---- dirty render ----
            if (dirty) {
                ui.render(present(state));
                dirty = false;
            }

            SDL_Delay(10);
        }

        LOG_INFO("Main") << "goodbye.";
        return 0;

    } catch (const std::exception& ex) {
        LOG_ERROR("Main") << "fatal: " << ex.what();
        return 1;
    }
}
