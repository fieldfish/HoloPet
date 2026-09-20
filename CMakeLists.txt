cmake_minimum_required(VERSION 3.20)
project(HoloPet VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

# ---- 选项 ----
option(BUILD_TESTS      "Build tests" OFF)
option(BUILD_HARDWARE   "Build Raspberry Pi hardware layer (requires libgpiod 2.x)" OFF)
option(BUILD_DISPLAY    "Build SDL2 display layer (requires SDL2 + SDL2_ttf)" OFF)
option(BUILD_INTEGRATED "Build integrated input/display application (requires BUILD_DISPLAY)" OFF)

# ---- V6 新增 (默认 OFF: 旧核心构建不依赖网络 SDK / Python) ----
option(BUILD_AI          "Build V6 AI application (requires SDL2 + SDL2_ttf)" OFF)
option(BUILD_AUDIO       "Build V6 USB audio bench entry (requires BUILD_AI)" OFF)
option(BUILD_PROJECTION  "Build V6 projection calibration mode (requires BUILD_AI)" OFF)

# ---- V6-R2: 依赖 guard 放顶层 (不嵌套在 if(BUILD_AI) 内, 保证可达) ----
if(BUILD_PROJECTION AND NOT BUILD_AI)
    message(FATAL_ERROR "BUILD_PROJECTION requires BUILD_AI=ON")
endif()
if(BUILD_AUDIO AND NOT BUILD_AI)
    message(FATAL_ERROR "BUILD_AUDIO requires BUILD_AI=ON")
endif()
if(BUILD_INTEGRATED AND NOT BUILD_DISPLAY)
    message(FATAL_ERROR "BUILD_INTEGRATED requires BUILD_DISPLAY=ON")
endif()

# ---- V1 纯逻辑核心 (始终编译) ----
set(CORE_HEADERS
    src/core/app_state.hpp
    src/core/app_controller.hpp
    src/input/input_event.hpp
    src/input/input_queue.hpp
    src/input/quadrature_decoder.hpp
    src/input/button_decoder.hpp
    src/input/encoder.hpp
    src/system/logger.hpp
)

add_executable(holopet
    src/main.cpp
    ${CORE_HEADERS}
)

target_include_directories(holopet PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---- V2 硬件层 ----
if(BUILD_HARDWARE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GPIODCXX REQUIRED IMPORTED_TARGET libgpiodcxx)

    set(HARDWARE_HEADERS
        src/hardware/gpio_config.hpp
        src/hardware/gpio_chip.hpp
        src/input/encoder_device.hpp
    )

    add_executable(holopet_encoder_test
        src/main_hardware.cpp
        ${HARDWARE_HEADERS}
        ${CORE_HEADERS}
    )

    target_include_directories(holopet_encoder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(holopet_encoder_test PRIVATE PkgConfig::GPIODCXX)

    find_package(Threads REQUIRED)
    target_link_libraries(holopet_encoder_test PRIVATE Threads::Threads)
endif()

# ---- V3 显示层 ----
if(BUILD_DISPLAY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

    set(DISPLAY_HEADERS
        src/display/display_config.hpp
        src/display/state_presenter.hpp
        src/display/holo_ui.hpp
    )

    add_executable(holopet_display_test
        src/main_display.cpp
        ${DISPLAY_HEADERS}
        ${CORE_HEADERS}
    )

    target_include_directories(holopet_display_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(holopet_display_test PRIVATE
        PkgConfig::SDL2
        PkgConfig::SDL2TTF
    )
endif()

# ---- V4 整合层 ----
if(BUILD_INTEGRATED)
    if(NOT BUILD_DISPLAY)
        message(FATAL_ERROR "BUILD_INTEGRATED requires BUILD_DISPLAY=ON")
    endif()

    add_executable(holopet_integrated
        src/main_integrated.cpp
        ${DISPLAY_HEADERS}
        ${CORE_HEADERS}
    )

    target_include_directories(holopet_integrated PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(holopet_integrated PRIVATE
        PkgConfig::SDL2
        PkgConfig::SDL2TTF
    )

    # 硬件模式: 额外 source + link
    if(BUILD_HARDWARE)
        target_sources(holopet_integrated PRIVATE ${HARDWARE_HEADERS})
        target_link_libraries(holopet_integrated PRIVATE PkgConfig::GPIODCXX)
        find_package(Threads REQUIRED)
        target_link_libraries(holopet_integrated PRIVATE Threads::Threads)
        target_compile_definitions(holopet_integrated PRIVATE HOLOPET_HAS_HARDWARE=1)
    endif()
endif()

# ---- V6 AI 纯逻辑头 (无外部依赖, 测试始终可用) ----
set(AI_HEADERS
    src/agent/agent_event.hpp
    src/agent/agent_config.hpp
    src/agent/conversation_controller.hpp
    src/agent/tool_policy.hpp
    src/ipc/ai_worker_protocol.hpp
    src/ipc/ai_worker_client.hpp
    src/display/expression_model.hpp
    src/display/font_selector.hpp
    src/display/projection_profile.hpp
    src/display/text_wrap.hpp
    src/audio/audio_config.hpp
    src/audio/audio_event.hpp
)

# ---- V6 AI 主程序 ----
if(BUILD_AI)
    # 双路径: Linux pkg-config / Windows SDL2 VC dev 包 (CMAKE_PREFIX_PATH)
    find_package(SDL2 QUIET CONFIG)
    if(SDL2_FOUND AND TARGET SDL2::SDL2)
        find_package(SDL2_ttf QUIET CONFIG)
        if(NOT SDL2_ttf_FOUND OR NOT TARGET SDL2_ttf::SDL2_ttf)
            message(FATAL_ERROR "SDL2 dev package found but SDL2_ttf missing; "
                                "set CMAKE_PREFIX_PATH to both VC dev packages")
        endif()
        set(HOLOPET_SDL_TARGET SDL2::SDL2)
        set(HOLOPET_SDLTTF_TARGET SDL2_ttf::SDL2_ttf)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
        set(HOLOPET_SDL_TARGET PkgConfig::SDL2)
        set(HOLOPET_SDLTTF_TARGET PkgConfig::SDL2TTF)
    endif()

    set(AI_SOURCES
        src/main_ai.cpp
        src/display/expression_renderer.hpp
        ${AI_HEADERS}
        ${CORE_HEADERS}
    )

    add_executable(holopet_ai ${AI_SOURCES})
    target_include_directories(holopet_ai PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(holopet_ai PRIVATE
        ${HOLOPET_SDL_TARGET}
        ${HOLOPET_SDLTTF_TARGET}
    )
    target_compile_definitions(holopet_ai PRIVATE HOLOPET_AI=1)

    # V6-R2: 宏必须实际控制源文件/功能入口 (HOLOPET_PROJECTION → 校准实现;
    # HOLOPET_AUDIO → audio bench 入口); 未启用的功能不得编译实现
    if(BUILD_PROJECTION)
        target_compile_definitions(holopet_ai PRIVATE HOLOPET_PROJECTION=1)
    endif()
    if(BUILD_AUDIO)
        target_compile_definitions(holopet_ai PRIVATE HOLOPET_AUDIO=1)
    endif()

    if(WIN32)
        target_link_libraries(holopet_ai PRIVATE ws2_32)
        # MSVC: SDL_main.h 将 main 重命名为 SDL_main → 必须链接 SDL2main
        if(TARGET SDL2::SDL2main)
            target_link_libraries(holopet_ai PRIVATE SDL2::SDL2main)
        endif()
        # R3: 运行时 DLL 随构建拷贝, 双击/脚本运行不再依赖 PATH
        add_custom_command(TARGET holopet_ai POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${HOLOPET_SDL_TARGET}>
                $<TARGET_FILE:${HOLOPET_SDLTTF_TARGET}>
                $<TARGET_FILE_DIR:holopet_ai>)
    endif()

    # 硬件模式: 与 V4 一致
    if(BUILD_HARDWARE)
        target_sources(holopet_ai PRIVATE ${HARDWARE_HEADERS})
        target_link_libraries(holopet_ai PRIVATE PkgConfig::GPIODCXX)
        find_package(Threads REQUIRED)
        target_link_libraries(holopet_ai PRIVATE Threads::Threads)
        target_compile_definitions(holopet_ai PRIVATE HOLOPET_HAS_HARDWARE=1)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(holopet_ai PRIVATE Threads::Threads)
    endif()
endif()

# ---- 测试 ----
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
