/**
 * main_integrated.cpp — HoloPet V4 输入+显示整合  V4-R1
 *
 * R1 修复:
 *  - EncoderDevice 使用 external queue → 真正单队列
 *  - Key repeat 过滤: Enter/Space/L 不响应自动重复
 *  - UI 提示与 V4 实际键位一致
 */

#include "core/app_controller.hpp"
#include "core/app_state.hpp"
#include "input/input_event.hpp"
#include "input/input_queue.hpp"
#include "display/display_config.hpp"
#include "display/state_presenter.hpp"
#include "display/holo_ui.hpp"
#include "system/logger.hpp"

#ifdef HOLOPET_HAS_HARDWARE
#include "input/encoder_device.hpp"
#include "hardware/gpio_config.hpp"
#include <csignal>
#endif

#include <SDL.h>

#include <string>
#include <atomic>

using namespace holopet;

#ifdef HOLOPET_HAS_HARDWARE
static volatile std::sig_atomic_t g_running = 1;
void signalHandler(int) { g_running = 0; }
#endif

static void pushKeyboardEvent(SDL_Keycode key, bool repeat, InputEventQueue& queue) {
    switch (key) {
        case SDLK_RIGHT:
        case SDLK_DOWN:  queue.push(EncoderEvent::Clockwise);        break;
        case SDLK_LEFT:
        case SDLK_UP:    queue.push(EncoderEvent::CounterClockwise); break;
        case SDLK_RETURN:
        case SDLK_SPACE:
            if (!repeat) queue.push(EncoderEvent::Pressed); break;
            break;
        case SDLK_l:
            if (!repeat) queue.push(EncoderEvent::LongPressed); break;
            break;
        default: break;
    }
}

static bool isQuitKey(SDL_Keycode key) {
    return key == SDLK_ESCAPE || key == SDLK_q;
}

int main(int argc, char* argv[]) {
    DisplayConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--fullscreen") {
            cfg.fullscreen = true;
        }
    }

    try {
        HoloUi ui(cfg);
        LOG_INFO("Main") << "HoloPet V4 Integrated ("
                         << (cfg.fullscreen ? "fullscreen" : "window") << ")";

        // 真正的单队列
        InputEventQueue queue;
        AppController controller;
        bool dirty = true;
        bool running = true;

#ifdef HOLOPET_HAS_HARDWARE
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        // V6-R2/R7: 生产 Pi 5 profile (三入口统一: A=27/B=17/SW=22, pinctrl-rp1 自动发现)
        GpioConfig gpio_cfg = GpioConfig::forPi5();
        EncoderDeviceConfig dev_cfg;
        dev_cfg.poll_interval = std::chrono::microseconds(1000);

        // V4: 使用外部队列 → 硬件事件直接进入 main queue
        EncoderDevice encoder(queue, dev_cfg, gpio_cfg);
        encoder.start();
#endif

        LOG_INFO("Main") << "Arrows: Move  Enter: OK  L: Home  Q: Quit";

        SDL_Event ev;

        while (running) {
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { running = false; break; }
                if (ev.type == SDL_KEYDOWN) {
                    if (isQuitKey(ev.key.keysym.sym)) { running = false; break; }
                    bool repeat = (ev.key.repeat != 0);
                    pushKeyboardEvent(ev.key.keysym.sym, repeat, queue);
                }
            }

#ifdef HOLOPET_HAS_HARDWARE
            if (!g_running) running = false;
#endif

            // —— 统一 drain 单一队列 ——
            EncoderEvent event;
            while (queue.tryPop(event)) {
                bool changed = controller.handleEvent(event);

                if (changed) {
                    dirty = true;
                    LOG_INFO("State") << "→ " << appStateName(controller.state());
                } else if (event == EncoderEvent::Pressed) {
                    LOG_INFO("Event") << "Confirmed: "
                                      << appStateName(controller.state());
                }
            }

            // —— Render ——
            if (dirty) {
                ui.render(present(controller.state()),
                          "Arrows: Move  Enter: OK  L: Home  Q: Quit");
                dirty = false;
            }

            SDL_Delay(10);
        }

#ifdef HOLOPET_HAS_HARDWARE
        LOG_INFO("Main") << "shutting down...";
        encoder.stop();
        encoder.join();
#endif

        LOG_INFO("Main") << "goodbye.";
        return 0;

    } catch (const std::exception& ex) {
        LOG_ERROR("Main") << "fatal: " << ex.what();
        return 1;
    }
}
