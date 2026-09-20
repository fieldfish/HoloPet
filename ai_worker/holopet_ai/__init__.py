"""holopet_ai — DEPRECATED 兼容入口 (V6-R3)

正式实现已迁移至 agent/holopet_agentd (单一 TurnRunner/协议/工具注册表)。
本包只做符号转发, 不再持有任何实现; 后续版本将移除。
新代码请直接使用: from holopet_agentd import ...
"""

import pathlib
import sys
import warnings

_AGENT_DIR = str(pathlib.Path(__file__).resolve().parents[2] / "agent")
if _AGENT_DIR not in sys.path:
    sys.path.insert(0, _AGENT_DIR)

warnings.warn("ai_worker.holopet_ai 已废弃, 请迁移到 agent/holopet_agentd",
              DeprecationWarning, stacklevel=2)

from holopet_agentd import protocol  # noqa: E402,F401
from holopet_agentd.main import (TurnRunner, WorkerServer, load_config,  # noqa: E402,F401
                                 main as agentd_main)

main = agentd_main

__version__ = "1.1.0"
__deprecated__ = True
