# 贡献指南

感谢你改进 HoloPet。

## 提交前

1. 从独立分支开发，保持改动范围清晰。
2. 不提交 Key、真实配置、录音、数据库、日志、模型权重或个人信息。
3. 新功能应补充相应测试和文档。
4. 机械改动应同时提交参数化 OpenSCAD 与导出的 STL，并写明单位和打印方向。
5. 不把桌面测试描述成 Raspberry Pi、硬件装配或光学实测通过。

## 基本检查

```bash
cmake -S . -B build-core -DBUILD_TESTS=ON
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
PYTHONPATH=agent python -m unittest discover -s agent/holopet_agentd/tests -t agent
```

## Pull Request

请说明：问题、修改方案、受影响模块、测试结果、仍未验证的设备条件。涉及 UI 时附截图；涉及机械时附尺寸表和切片预览。贡献内容按仓库的 Apache License 2.0 提交。
