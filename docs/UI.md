# 表情与菜单界面

## 视觉规范

- 背景：纯黑 `RGB(0,0,0)`；
- 主色：蓝色 `RGB(64,160,255)`；
- 次级蓝：`RGB(32,128,216)`；
- 不使用白色、渐变、辉光、腮红、眉毛或来源不明的角色素材；
- 所有表情和菜单图标由 SDL2 图元实时绘制。

## 表情系统

生产表情包括：开心、惊讶、馋、为难、无语、喜悦、生气、疑问和可爱。状态与情绪分离：Listening、Thinking、Speaking、Error 和 Offline 会在情绪基调上叠加眼睛、视线或嘴型变化。

空闲状态约每 4.5–8 秒随机眨眼，并有小概率连续眨眼；思考状态使用有界视线偏移和小噘嘴；说话状态使用真实 TTS 音频电平驱动嘴型。

实现位置：

- `src/display/expression_model.hpp`
- `src/display/expression_animation.hpp`
- `src/display/expression_renderer.hpp`

![九种表情](media/expression_set.svg)

## 方形列表菜单

长按 EC11 进入菜单。旋转切换项目，短按确认，长按返回；长时间无操作自动退出。根菜单包含：

1. 时钟/返回表情；
2. 定时器；
3. 闹钟；
4. 便签；
5. 模型模式。

定时器和闹钟列表可创建或管理实际条目，便签页可读取正文预览，模型页提供快速、自动、深度和本地四种模式。

实现位置：

- `src/ui/menu_controller.hpp`
- `src/ui/square_menu_renderer.hpp`
- `src/main_ai.cpp`

![方形菜单](media/menu_square.svg)
