"""holopet_agentd.router — R8_R4 模型路由决策 (集中可单测)。"""
from .route_decision import (route_decision, detect_explicit_override,
                             detect_insufficient,
                             ROUTE_LOCAL_TOOL, ROUTE_FAST, ROUTE_STRONG,
                             ROUTE_LOCAL_LLM)

__all__ = ["route_decision", "detect_explicit_override",
           "detect_insufficient", "ROUTE_LOCAL_TOOL", "ROUTE_FAST",
           "ROUTE_STRONG", "ROUTE_LOCAL_LLM"]
