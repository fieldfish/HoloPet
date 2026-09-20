# -*- coding: utf-8 -*-
"""agent/confirmation.py — R5_R4 工作包 C: 二次确认状态机。

确认规则：
  1. 首轮只生成 pending_confirmation, 不执行副作用;
  2. 屏幕和 TTS 明确询问一次;
  3. 待确认记录绑定: 会话、工具名、规范化参数哈希和原始 request;
  4. 有效期 30 秒;
  5. 仅"确认/是/执行"等明确肯定词接受; "取消/不要/算了"取消; 其他输入
     重新询问但不延长超过 30 秒;
  6. 确认后使用原规范化参数执行恰好一次, 不让模型重新生成另一组参数;
  7. 超时返回 confirmation_expired;
  8. 重启后待确认状态默认失效 (仅内存态, 不持久化);
  9. 取消动作可随时终止待确认;
 10. 审计记录参数摘要, 不记录被删除便签正文。
"""
from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, field
from typing import Optional

TTL_SECONDS = 30.0

ACCEPT_WORDS = ("确认", "确定", "是", "是的", "执行", "好的", "可以", "行")
CANCEL_WORDS = ("取消", "不要", "算了", "不", "别", "放弃")


@dataclass
class PendingConfirmation:
    request_id: str
    tool_name: str
    args_hash: str
    raw_request: dict                    # 原始 request (规范化前)
    normalized_args: dict                # 确认后原样执行, 不再让模型重生成
    tool_call_id: str
    created_ms: int
    expires_ms: int
    asked_once: bool = field(default=False)


class ConfirmationGate:
    """单活动待确认槽位 (每轮最多一个; 新请求覆盖旧请求 = 旧请求作废)。"""

    def __init__(self, ttl_seconds: float = TTL_SECONDS,
                 clock=None):
        self._ttl = ttl_seconds
        self._clock = clock or time.monotonic
        self._pending: Optional[PendingConfirmation] = None

    @property
    def pending(self) -> Optional[PendingConfirmation]:
        return self._pending

    @staticmethod
    def normalized_args_hash(normalized_args: dict) -> str:
        blob = json.dumps(normalized_args, sort_keys=True,
                          ensure_ascii=False).encode("utf-8")
        return hashlib.sha256(blob).hexdigest()

    def request(self, *, request_id: str, tool_name: str,
                normalized_args: dict, raw_request: dict,
                tool_call_id: str) -> PendingConfirmation:
        """登记待确认 (覆盖旧的); 返回记录。不执行副作用。"""
        now_ms = int(self._clock() * 1000)
        p = PendingConfirmation(
            request_id=request_id,
            tool_name=tool_name,
            args_hash=self.normalized_args_hash(normalized_args),
            raw_request=raw_request,
            normalized_args=normalized_args,
            tool_call_id=tool_call_id,
            created_ms=now_ms,
            expires_ms=now_ms + int(self._ttl * 1000),
        )
        self._pending = p
        return p

    def interpret(self, user_text: str, now_ms: int) -> str:
        """用户输入解释: accept / cancel / ask_again。过期优先级最高。"""
        if self._pending is None:
            return "no_pending"
        if now_ms >= self._pending.expires_ms:
            return "expired"
        t = (user_text or "").strip()
        low = t.lower()
        for w in ACCEPT_WORDS:
            if t == w or low == w:
                return "accept"
        for w in CANCEL_WORDS:
            if t == w or low == w:
                return "cancel"
        return "ask_again"

    def accept(self) -> Optional[PendingConfirmation]:
        """确认: 取出记录并清空槽位 (调用方据此执行恰好一次)。"""
        p = self._pending
        self._pending = None
        return p

    def cancel(self) -> Optional[PendingConfirmation]:
        p = self._pending
        self._pending = None
        return p

    def expire(self) -> Optional[PendingConfirmation]:
        """过期: 返回过期记录供审计 (code=confirmation_expired)。"""
        p = self._pending
        self._pending = None
        return p

    def clear(self) -> None:
        self._pending = None
