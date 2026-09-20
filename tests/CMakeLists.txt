# 测试目标

add_library(test_runner INTERFACE)
target_include_directories(test_runner INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(test_runner INTERFACE cxx_std_20)

find_package(Threads REQUIRED)

# ---- V1 ----
add_executable(test_app_state test_app_state.cpp)
target_link_libraries(test_app_state PRIVATE test_runner Threads::Threads)
add_test(NAME test_app_state COMMAND test_app_state)

add_executable(test_quadrature_decoder test_quadrature_decoder.cpp)
target_link_libraries(test_quadrature_decoder PRIVATE test_runner)
add_test(NAME test_quadrature_decoder COMMAND test_quadrature_decoder)

add_executable(test_button_decoder test_button_decoder.cpp)
target_link_libraries(test_button_decoder PRIVATE test_runner)
add_test(NAME test_button_decoder COMMAND test_button_decoder)

# ---- V2 ----
add_executable(test_gpio_config test_gpio_config.cpp)
target_link_libraries(test_gpio_config PRIVATE test_runner)
add_test(NAME test_gpio_config COMMAND test_gpio_config)

# ---- V3 ----
add_executable(test_display_config test_display_config.cpp)
target_link_libraries(test_display_config PRIVATE test_runner)
add_test(NAME test_display_config COMMAND test_display_config)

add_executable(test_state_presenter test_state_presenter.cpp)
target_link_libraries(test_state_presenter PRIVATE test_runner)
add_test(NAME test_state_presenter COMMAND test_state_presenter)

# ---- V4 ----
add_executable(test_app_controller test_app_controller.cpp)
target_link_libraries(test_app_controller PRIVATE test_runner)
add_test(NAME test_app_controller COMMAND test_app_controller)

add_executable(test_event_pipeline test_event_pipeline.cpp)
target_link_libraries(test_event_pipeline PRIVATE test_runner)
add_test(NAME test_event_pipeline COMMAND test_event_pipeline)

# ---- V6 (纯逻辑新增; 不依赖 SDL/网络/Python) ----
add_executable(test_pi5_profile test_pi5_profile.cpp)
target_link_libraries(test_pi5_profile PRIVATE test_runner)
add_test(NAME test_pi5_profile COMMAND test_pi5_profile)

# ---- R8_R2 (工作包 E): 完整回答分页器 (纯逻辑) ----
add_executable(test_text_pager test_text_pager.cpp)
target_link_libraries(test_text_pager PRIVATE test_runner)
add_test(NAME test_text_pager COMMAND test_text_pager)
set_tests_properties(test_text_pager PROPERTIES TIMEOUT 30)

# ---- R8_R2 (工作包 D): 文字 typed command / 队列 payload / 文本文件校验 ----
add_executable(test_submit_text_command test_submit_text_command.cpp)
target_link_libraries(test_submit_text_command PRIVATE test_runner Threads::Threads)
if(WIN32)
    target_link_libraries(test_submit_text_command PRIVATE ws2_32)
endif()
add_test(NAME test_submit_text_command COMMAND test_submit_text_command)
set_tests_properties(test_submit_text_command PROPERTIES TIMEOUT 30)

# ---- R8_R1 (A3 回归): GPIO 候选枚举去重 (需 libgpiod 的 gpio_chip.hpp,
#      仅在 BUILD_HARDWARE 构建注册; Windows 默认 OFF 不编译) ----
if(BUILD_HARDWARE)
    add_executable(test_gpio_chip_dedup test_gpio_chip_dedup.cpp)
    target_link_libraries(test_gpio_chip_dedup PRIVATE test_runner)
    add_test(NAME test_gpio_chip_dedup COMMAND test_gpio_chip_dedup)
endif()

add_executable(test_runtime_state test_runtime_state.cpp)
target_link_libraries(test_runtime_state PRIVATE test_runner)
add_test(NAME test_runtime_state COMMAND test_runtime_state)

add_executable(test_conversation_controller test_conversation_controller.cpp)
target_link_libraries(test_conversation_controller PRIVATE test_runner)
add_test(NAME test_conversation_controller COMMAND test_conversation_controller)

add_executable(test_conversation_cancel test_conversation_cancel.cpp)
target_link_libraries(test_conversation_cancel PRIVATE test_runner)
add_test(NAME test_conversation_cancel COMMAND test_conversation_cancel)

add_executable(test_expression_model test_expression_model.cpp)
target_link_libraries(test_expression_model PRIVATE test_runner)
add_test(NAME test_expression_model COMMAND test_expression_model)

# ---- R8_R4 (B1): 表情微动画控制器 (纯逻辑, 确定性) ----
add_executable(test_expression_animation test_expression_animation.cpp)
target_link_libraries(test_expression_animation PRIVATE test_runner)
add_test(NAME test_expression_animation COMMAND test_expression_animation)

# ---- R8_R4 (B2): 本地功能服务 (纯逻辑, 注入时钟/目录) ----
add_executable(test_local_feature_service test_local_feature_service.cpp)
target_link_libraries(test_local_feature_service PRIVATE test_runner)
add_test(NAME test_local_feature_service COMMAND test_local_feature_service)

# ---- R8_R4 (B3): EC11 菜单状态机 (纯逻辑) ----
add_executable(test_menu_controller test_menu_controller.cpp)
target_link_libraries(test_menu_controller PRIVATE test_runner)
add_test(NAME test_menu_controller COMMAND test_menu_controller)

# ---- R8_R4 (B4): 工具协议桥 (纯逻辑, 幂等/错误/共用服务) ----
add_executable(test_tool_bridge test_tool_bridge.cpp)
target_link_libraries(test_tool_bridge PRIVATE test_runner)
add_test(NAME test_tool_bridge COMMAND test_tool_bridge)

# ---- R8_R4 (B6): 工具请求线级协议 ----
add_executable(test_tool_wire test_tool_wire.cpp)
target_link_libraries(test_tool_wire PRIVATE test_runner)
add_test(NAME test_tool_wire COMMAND test_tool_wire)

add_executable(test_tool_policy test_tool_policy.cpp)
target_link_libraries(test_tool_policy PRIVATE test_runner)
add_test(NAME test_tool_policy COMMAND test_tool_policy)

add_executable(test_projection_profile test_projection_profile.cpp)
target_link_libraries(test_projection_profile PRIVATE test_runner)
add_test(NAME test_projection_profile COMMAND test_projection_profile)

add_executable(test_ai_worker_protocol test_ai_worker_protocol.cpp)
target_link_libraries(test_ai_worker_protocol PRIVATE test_runner)
add_test(NAME test_ai_worker_protocol COMMAND test_ai_worker_protocol)

add_executable(test_ai_worker_client test_ai_worker_client.cpp)
target_link_libraries(test_ai_worker_client PRIVATE test_runner)
add_test(NAME test_ai_worker_client COMMAND test_ai_worker_client)

add_executable(test_text_wrap test_text_wrap.cpp)
target_link_libraries(test_text_wrap PRIVATE test_runner)
add_test(NAME test_text_wrap COMMAND test_text_wrap)

add_executable(test_agent_config test_agent_config.cpp)
target_link_libraries(test_agent_config PRIVATE test_runner)
add_test(NAME test_agent_config COMMAND test_agent_config)

add_executable(test_audio_config test_audio_config.cpp)
target_link_libraries(test_audio_config PRIVATE test_runner)
add_test(NAME test_audio_config COMMAND test_audio_config)

# ---- V6-R2 (返工新增) ----
add_executable(test_mini_json test_mini_json.cpp)
target_link_libraries(test_mini_json PRIVATE test_runner)
add_test(NAME test_mini_json COMMAND test_mini_json)

add_executable(test_profile_json test_profile_json.cpp)
target_link_libraries(test_profile_json PRIVATE test_runner)
add_test(NAME test_profile_json COMMAND test_profile_json)

add_executable(test_pixel_pipeline test_pixel_pipeline.cpp)
target_link_libraries(test_pixel_pipeline PRIVATE test_runner)
add_test(NAME test_pixel_pipeline COMMAND test_pixel_pipeline)

add_executable(test_projection_geometry test_projection_geometry.cpp)
target_link_libraries(test_projection_geometry PRIVATE test_runner)
add_test(NAME test_projection_geometry COMMAND test_projection_geometry)

add_executable(test_hardware_pipeline test_hardware_pipeline.cpp)
target_link_libraries(test_hardware_pipeline PRIVATE test_runner)
add_test(NAME test_hardware_pipeline COMMAND test_hardware_pipeline)

add_executable(test_worker_session test_worker_session.cpp)
target_link_libraries(test_worker_session PRIVATE test_runner)
if(WIN32)
    target_link_libraries(test_worker_session PRIVATE ws2_32)
endif()
add_test(NAME test_worker_session COMMAND test_worker_session)

# ---- V6-R3 (退出语义 / 工作包 C) ----
add_executable(test_shutdown_queue_drain test_shutdown_queue_drain.cpp)
target_link_libraries(test_shutdown_queue_drain PRIVATE test_runner Threads::Threads)
if(WIN32)
    target_link_libraries(test_shutdown_queue_drain PRIVATE ws2_32)
endif()
add_test(NAME test_shutdown_queue_drain COMMAND test_shutdown_queue_drain)
set_tests_properties(test_shutdown_queue_drain PROPERTIES TIMEOUT 30)

# ---- V6-R1 (缺陷 D2: 异常路径收尾) ----
add_executable(test_exception_cleanup test_exception_cleanup.cpp)
target_link_libraries(test_exception_cleanup PRIVATE test_runner Threads::Threads)
if(WIN32)
    target_link_libraries(test_exception_cleanup PRIVATE ws2_32)
endif()
add_test(NAME test_exception_cleanup COMMAND test_exception_cleanup)
set_tests_properties(test_exception_cleanup PROPERTIES TIMEOUT 30)

# ---- V6-R4-R2 (F1: --worker-* 参数解析) ----
add_executable(test_worker_args test_worker_args.cpp)
target_link_libraries(test_worker_args PRIVATE test_runner)
add_test(NAME test_worker_args COMMAND test_worker_args)
set_tests_properties(test_worker_args PROPERTIES TIMEOUT 30)

# ---- V6-R4-R1 (P4: 退出安全与 UDS) ----
add_executable(test_command_queue_shutdown_priority test_command_queue_shutdown_priority.cpp)
target_link_libraries(test_command_queue_shutdown_priority PRIVATE test_runner)
add_test(NAME test_command_queue_shutdown_priority COMMAND test_command_queue_shutdown_priority)
set_tests_properties(test_command_queue_shutdown_priority PROPERTIES TIMEOUT 30)

add_executable(test_exit_guard_move_only test_exit_guard_move_only.cpp)
target_link_libraries(test_exit_guard_move_only PRIVATE test_runner)
add_test(NAME test_exit_guard_move_only COMMAND test_exit_guard_move_only)
set_tests_properties(test_exit_guard_move_only PROPERTIES TIMEOUT 30)

add_executable(test_exit_guard_fault_injection test_exit_guard_fault_injection.cpp)
target_link_libraries(test_exit_guard_fault_injection PRIVATE test_runner Threads::Threads)
if(WIN32)
    target_link_libraries(test_exit_guard_fault_injection PRIVATE ws2_32)
endif()
add_test(NAME test_exit_guard_fault_injection COMMAND test_exit_guard_fault_injection)
set_tests_properties(test_exit_guard_fault_injection PROPERTIES TIMEOUT 30)

# Linux 实跑 (Windows 运行时 SKIP): 真实 C++ client ↔ Python agentd UDS
add_executable(test_uds_cpp_roundtrip test_uds_cpp_roundtrip.cpp)
target_link_libraries(test_uds_cpp_roundtrip PRIVATE test_runner Threads::Threads)
if(WIN32)
    target_link_libraries(test_uds_cpp_roundtrip PRIVATE ws2_32)
endif()
# R7 (A1): 测试不得依赖调用方 cwd — 编译期注入绝对源码根目录
target_compile_definitions(test_uds_cpp_roundtrip PRIVATE
    HOLOPET_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME test_uds_cpp_roundtrip COMMAND test_uds_cpp_roundtrip)
set_tests_properties(test_uds_cpp_roundtrip PROPERTIES TIMEOUT 60)
# R7_R2 (任务 A): 生命周期驱动器 — 精确退出码/marker/零残留/三 cwd/并发/
# 故障注入 (after_tmp=20, after_spawn=21, agent-dir 无效=10) 由驱动器断言,
# 取代 R7_R1 的 WILL_FAIL 负向 (任意非零退出不再被接受)。Windows 下驱动器
# 只跑 agent-dir 无效场景 (exit 10), AF_UNIX 场景明确 SKIP。
# QUIET: 不打印 CMake 解析出的解释器绝对路径 (含用户名, 会污染正式日志/隐私扫描)
find_package(Python3 REQUIRED COMPONENTS Interpreter QUIET)
add_test(NAME test_uds_cpp_lifecycle_driver
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_uds_cpp_lifecycle_driver.py
            $<TARGET_FILE:test_uds_cpp_roundtrip>
            --project-root ${CMAKE_SOURCE_DIR})
set_tests_properties(test_uds_cpp_lifecycle_driver PROPERTIES TIMEOUT 180)

# ---- V6-R4-R1 (P1: 回答/时钟/表情 产品链) ----
add_executable(test_content_pipeline test_content_pipeline.cpp)
target_link_libraries(test_content_pipeline PRIVATE test_runner)
add_test(NAME test_content_pipeline COMMAND test_content_pipeline)
set_tests_properties(test_content_pipeline PROPERTIES TIMEOUT 30)

add_executable(test_thinking_deadline test_thinking_deadline.cpp)
target_link_libraries(test_thinking_deadline PRIVATE test_runner)
add_test(NAME test_thinking_deadline COMMAND test_thinking_deadline)
set_tests_properties(test_thinking_deadline PROPERTIES TIMEOUT 30)

add_executable(test_expression_tool_flow test_expression_tool_flow.cpp)
target_link_libraries(test_expression_tool_flow PRIVATE test_runner)
add_test(NAME test_expression_tool_flow COMMAND test_expression_tool_flow)
set_tests_properties(test_expression_tool_flow PROPERTIES TIMEOUT 30)

# ---- V6-R4 (P1: 跨语言 IPC v1 合同, 共享 fixture) ----
add_executable(test_ipc_contract test_ipc_contract.cpp)
target_link_libraries(test_ipc_contract PRIVATE test_runner)
add_test(NAME test_ipc_contract COMMAND test_ipc_contract
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ipc_v1/cases.json)
set_tests_properties(test_ipc_contract PROPERTIES TIMEOUT 30)

# ---- R8_R2_R1 (A+B 返工): 文字单轮生命周期状态机 / 逐页展示证据 /
#      原子事件审计 (纯逻辑, 无 SDL 依赖; 归入 AI 套件 — 该套件承载
#      文字轮生命周期, Core 套件维持 40 项不变) ----
if(BUILD_AI)
    add_executable(test_text_turn_lifecycle test_text_turn_lifecycle.cpp)
    target_link_libraries(test_text_turn_lifecycle PRIVATE test_runner)
    add_test(NAME test_text_turn_lifecycle COMMAND test_text_turn_lifecycle)
    set_tests_properties(test_text_turn_lifecycle PROPERTIES TIMEOUT 30)
endif()

# ---- V6-R2: 离屏渲染验收 (仅 BUILD_AI, 需要 SDL) ----
if(BUILD_AI)
    add_executable(test_projection_render test_projection_render.cpp)
    target_link_libraries(test_projection_render PRIVATE test_runner
        ${HOLOPET_SDL_TARGET} ${HOLOPET_SDLTTF_TARGET})
    target_compile_definitions(test_projection_render PRIVATE HOLOPET_PROJECTION=1)
    if(WIN32)
        target_link_libraries(test_projection_render PRIVATE ws2_32)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(test_projection_render PRIVATE SDL2::SDL2main)
        endif()
    endif()
    add_test(NAME test_projection_render COMMAND test_projection_render)
    # R3: R2 该测试在 ctest 下不可终止 (无 TIMEOUT, 挂起后 stowed exception);
    # 超时即 FAIL, 不允许提高到数分钟或移除该门。
    set_tests_properties(test_projection_render PROPERTIES TIMEOUT 20)
    # R3 根因修复: ctest 环境 PATH 无 SDL2.dll 时, Windows loader 弹
    # "缺少 DLL" 对话框 → 进程挂起 (即 R2 的“稳定挂住”)。
    # 构建后把运行时 DLL 拷到测试可执行文件目录, 消除对 PATH 的依赖。
    if(WIN32 AND HOLOPET_SDL_TARGET)
        add_custom_command(TARGET test_projection_render POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${HOLOPET_SDL_TARGET}>
                $<TARGET_FILE:${HOLOPET_SDLTTF_TARGET}>
                $<TARGET_FILE_DIR:test_projection_render>)
    endif()
endif()

# ---- R8_R4 (B1): 表情动画帧散列 (仅 BUILD_AI, 离屏 SDL) ----
if(BUILD_AI)
    add_executable(test_face_animation_render test_face_animation_render.cpp)
    target_link_libraries(test_face_animation_render PRIVATE test_runner
        ${HOLOPET_SDL_TARGET} ${HOLOPET_SDLTTF_TARGET})
    if(WIN32)
        target_link_libraries(test_face_animation_render PRIVATE ws2_32)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(test_face_animation_render PRIVATE SDL2::SDL2main)
        endif()
    endif()
    add_test(NAME test_face_animation_render COMMAND test_face_animation_render)
    set_tests_properties(test_face_animation_render PROPERTIES TIMEOUT 20)
    if(WIN32 AND HOLOPET_SDL_TARGET)
        add_custom_command(TARGET test_face_animation_render POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${HOLOPET_SDL_TARGET}>
                $<TARGET_FILE:${HOLOPET_SDLTTF_TARGET}>
                $<TARGET_FILE_DIR:test_face_animation_render>)
    endif()
endif()

# ---- R8_R4_R3_R4_R5: 方形菜单渲染确定性测试 (仅 BUILD_AI, 离屏 SDL) ----
if(BUILD_AI)
    add_executable(test_square_menu_renderer test_square_menu_renderer.cpp)
    target_link_libraries(test_square_menu_renderer PRIVATE test_runner
        ${HOLOPET_SDL_TARGET} ${HOLOPET_SDLTTF_TARGET})
    if(WIN32)
        target_link_libraries(test_square_menu_renderer PRIVATE ws2_32)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(test_square_menu_renderer PRIVATE SDL2::SDL2main)
        endif()
    endif()
    add_test(NAME test_square_menu_renderer COMMAND test_square_menu_renderer)
    set_tests_properties(test_square_menu_renderer PROPERTIES TIMEOUT 20)
    if(WIN32 AND HOLOPET_SDL_TARGET)
        add_custom_command(TARGET test_square_menu_renderer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${HOLOPET_SDL_TARGET}>
                $<TARGET_FILE:${HOLOPET_SDLTTF_TARGET}>
                $<TARGET_FILE_DIR:test_square_menu_renderer>)
    endif()
endif()

# ---- R8_R2_R7: CJK 字形选择行为 (纯逻辑; 不依赖 SDL/字体安装) ----
add_executable(test_font_selector test_font_selector.cpp)
target_link_libraries(test_font_selector PRIVATE test_runner)
add_test(NAME test_font_selector COMMAND test_font_selector)
set_tests_properties(test_font_selector PROPERTIES TIMEOUT 20)
