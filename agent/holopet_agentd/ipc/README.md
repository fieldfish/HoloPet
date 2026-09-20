# ipc/ — 传输与协议 (骨架, V6-R1)

职责: Unix Domain Socket (生产) / loopback TCP (Windows 开发回退) 的
监听与连接管理; 行缓冲 (半包/粘包); 协议版本协商。

- 现行实现: `../protocol.py` (v1: 结构化拒绝/行长上限/稳定错误码) 与
  `../main.py` 的 `WorkerServer._serve_uds/_serve_tcp`。
- R4 计划: 把传输层从 main.py 迁入本包 (窄迁移, 不改语义);
  UDS roundtrip 测试: `../tests/test_uds_roundtrip.py`。

不得在此复制第二套 TurnRunner/协议。
