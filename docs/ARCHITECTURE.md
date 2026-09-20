# 架构说明

## 设计目标

HoloPet 将硬实时要求较低但必须稳定的设备逻辑放在 C++ 进程，将模型访问、语音 Provider 和工具编排放在 Python 服务。两者通过版本化 JSON Lines 协议通信，避免 UI 主线程直接等待网络请求。

## C++ 设备运行时

主要职责：

- SDL2 显示、字体选择、表情动画和分页内容；
- EC11 旋钮输入、短按/长按判定与菜单状态机；
- 定时器、闹钟、便签、时钟模式和提示横幅；
- Unix Socket/TCP 开发回退客户端；
- 对话状态、取消、超时与进程退出顺序；
- Raspberry Pi GPIO 所有权。

主要入口和模块：

- `src/main_ai.cpp`
- `src/agent/conversation_controller.hpp`
- `src/display/expression_model.hpp`
- `src/display/expression_animation.hpp`
- `src/display/expression_renderer.hpp`
- `src/ui/menu_controller.hpp`
- `src/ui/square_menu_renderer.hpp`
- `src/features/local_feature_service.hpp`
- `src/features/tool_bridge.hpp`

## Python 对话服务

主要职责：

- 录音、ASR、模型请求、TTS 与播放；
- 快速/深度/本地模型路由；
- 工具白名单、参数校验、执行预算和超时；
- 破坏性动作二次确认；
- 有界会话摘要、显式偏好和幂等记录；
- 统一错误码与审计摘要。

主要入口和模块：

- `agent/holopet_agentd/main.py`
- `agent/holopet_agentd/agent/runtime.py`
- `agent/holopet_agentd/agent/session_store.py`
- `agent/holopet_agentd/providers/`
- `agent/holopet_agentd/router/`
- `agent/holopet_agentd/tools/`

## 一次语音交互

```text
旋钮短按
  → C++ start_turn
  → Python listening
  → ALSA 录音
  → ASR transcript
  → 模型输出 content/tool_call
  → C++ 执行设备工具并回传 tool_result
  → 模型生成最终回答
  → C++ 分页显示与表情更新
  → TTS 生成并播放
  → done / Idle
```

取消是协议的一部分。取消后不应继续执行迟到的工具调用、TTS 或历史写入。

## 工具边界

工具目录记录执行位置、是否产生副作用、确认策略和超时。设备状态相关工具由 C++ 执行；偏好和会话相关工具由 Python 执行。所有工具调用必须经过白名单与参数校验。

## 本地模型

本地模型通过 OpenAI-compatible HTTP 接口接入。仓库只包含启动和测量脚本，不包含模型权重。离线模式必须阻止云端请求。
