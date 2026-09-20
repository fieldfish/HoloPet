# -*- coding: utf-8 -*-
"""agent/session_store.py — R5_R4 工作包 D: 持久会话与有限记忆 (SQLite)。

存储规则：
  - agent_state.sqlite3 位于应用数据目录, 不进入版本库或公开发行包; 权限 0600;
  - 有界会话摘要 / 显式用户偏好 / 已完成轮次幂等摘要 / schema_version;
  - 不保存原始 WAV/PCM、Key、Authorization; 默认不保存完整逐字对话;
  - 快速上下文最多最近 8 个有效轮次 (由调用方使用, 本表只存摘要);
  - 会话摘要最多 100 条或 30 天; 偏好最多 100 条;
  - 幂等记录保留至少 24 小时, 最多 500 条;
  - 损坏恢复: 备份损坏文件以便诊断, 然后重建 + 迁移。
"""
from __future__ import annotations

import hashlib
import os
import sqlite3
import time
from typing import Optional, Tuple

SCHEMA_VERSION = 1
SUMMARY_MAX = 100
SUMMARY_TTL_MS = 30 * 24 * 3600 * 1000
PREF_MAX = 100
IDEM_MAX = 500
IDEM_TTL_MS = 24 * 3600 * 1000


def digest_of(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


class StoreCorrupted(RuntimeError):
    pass


class AgentStore:
    """SQLite 持久层 (0600; 事务; 损坏恢复; 容量/TTL 上限)。"""

    def __init__(self, path: str, clock=None):
        self._path = path
        self._clock = clock or (lambda: time.time() * 1000)
        self._conn: Optional[sqlite3.Connection] = None

    # ---- 生命周期 ----
    def open(self) -> None:
        """连接 + 迁移; 损坏时备份后重建。"""
        parent = os.path.dirname(self._path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        try:
            self._conn = sqlite3.connect(self._path, timeout=5.0)
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._migrate()
        except sqlite3.DatabaseError as e:
            self._recover_from_corruption(e)
        if os.path.exists(self._path):
            try:
                os.chmod(self._path, 0o600)
            except OSError:
                pass

    def close(self) -> None:
        if self._conn is not None:
            try:
                self._conn.close()
            except sqlite3.Error:
                pass
            self._conn = None

    def _recover_from_corruption(self, orig: Exception) -> None:
        """备份损坏文件（不删除，便于诊断），然后重建并迁移。"""
        if self._conn is not None:
            try:
                self._conn.close()
            except sqlite3.Error:
                pass
            self._conn = None
        backup = "%s.corrupt-%d" % (self._path, int(self._clock()))
        try:
            if os.path.exists(self._path):
                os.replace(self._path, backup)
        except OSError:
            pass
        try:
            self._conn = sqlite3.connect(self._path, timeout=5.0)
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._migrate()
        except sqlite3.DatabaseError as e2:
            raise StoreCorrupted(
                "recreate failed after %r: %r" % (orig, e2)) from e2

    def _migrate(self) -> None:
        with self._conn:
            self._conn.execute(
                "CREATE TABLE IF NOT EXISTS schema_version ("
                "version INTEGER NOT NULL)")
            cur = self._conn.execute(
                "SELECT COUNT(*) FROM schema_version").fetchone()
            if cur and cur[0] == 0:
                self._conn.execute(
                    "INSERT INTO schema_version(version) VALUES (?)",
                    (SCHEMA_VERSION,))
            self._conn.execute(
                "CREATE TABLE IF NOT EXISTS session_summaries ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "request_id TEXT NOT NULL,"
                "answer_digest TEXT NOT NULL,"
                "created_ms INTEGER NOT NULL)")
            self._conn.execute(
                "CREATE TABLE IF NOT EXISTS preferences ("
                "key TEXT PRIMARY KEY,"
                "value TEXT NOT NULL,"
                "created_ms INTEGER NOT NULL,"
                "updated_ms INTEGER NOT NULL)")
            self._conn.execute(
                "CREATE TABLE IF NOT EXISTS idempotency ("
                "request_id TEXT NOT NULL,"
                "tool_call_id TEXT NOT NULL,"
                "args_hash TEXT NOT NULL,"
                "result_code TEXT NOT NULL,"
                "result_digest TEXT NOT NULL,"
                "created_ms INTEGER NOT NULL,"
                "PRIMARY KEY(request_id, tool_call_id))")

    # ---- 会话摘要 (有界) ----
    def add_summary(self, request_id: str, answer_text: str) -> None:
        with self._conn:
            self._conn.execute(
                "INSERT INTO session_summaries"
                "(request_id, answer_digest, created_ms) VALUES (?,?,?)",
                (request_id, digest_of(answer_text), int(self._clock())))
        self._prune_summaries()          # 先插后剪, 保证 ≤ 上限

    def _prune_summaries(self) -> None:
        ttl_cut = int(self._clock()) - SUMMARY_TTL_MS
        with self._conn:
            self._conn.execute(
                "DELETE FROM session_summaries WHERE created_ms < ?",
                (ttl_cut,))
            self._conn.execute(
                "DELETE FROM session_summaries WHERE id NOT IN ("
                "SELECT id FROM session_summaries ORDER BY id DESC "
                "LIMIT ?)", (SUMMARY_MAX,))

    def summary_count(self) -> int:
        return self._conn.execute(
            "SELECT COUNT(*) FROM session_summaries").fetchone()[0]

    # ---- 偏好 ----
    def remember_preference(self, key: str, value: str) -> str:
        """写入偏好; 超上限返回 preference_limit_exceeded (fail closed)。"""
        if not key.strip() or not value.strip():
            return "invalid_preference"
        now = int(self._clock())
        with self._conn:
            exists = self._conn.execute(
                "SELECT 1 FROM preferences WHERE key=?", (key,)).fetchone()
            if not exists:
                n = self._conn.execute(
                    "SELECT COUNT(*) FROM preferences").fetchone()[0]
                if n >= PREF_MAX:
                    return "preference_limit_exceeded"
                self._conn.execute(
                    "INSERT INTO preferences"
                    "(key, value, created_ms, updated_ms) VALUES (?,?,?,?)",
                    (key, value, now, now))
            else:
                self._conn.execute(
                    "UPDATE preferences SET value=?, updated_ms=? "
                    "WHERE key=?", (value, now, key))
        return "ok"

    def list_preferences(self, limit: int = 100) -> list:
        rows = self._conn.execute(
            "SELECT key, value FROM preferences ORDER BY key LIMIT ?",
            (limit,)).fetchall()
        return [{"key": k, "value": v} for k, v in rows]

    def forget_preference(self, key: str) -> str:
        """key='all' 需要确认后由调用方以 forget_all 执行; 单条直接删。"""
        if key == "all":
            return "confirm_required"
        with self._conn:
            cur = self._conn.execute(
                "DELETE FROM preferences WHERE key=?", (key,))
        return "ok" if cur.rowcount else "not_found"

    def forget_all_preferences(self) -> None:
        with self._conn:
            self._conn.execute("DELETE FROM preferences")

    # ---- 幂等 (副作用工具) ----
    def record_idempotent(self, *, request_id: str, tool_call_id: str,
                          args_hash: str, result_code: str,
                          result_digest: str) -> Tuple[str, dict]:
        """登记副作用执行结果。

        返回 ("stored", {}) 或 ("duplicate", 先前的 {result_code,
        result_digest, created_ms})。同一 (request_id, tool_call_id) 的
        重放只返回先前结果, 不再执行副作用。
        """
        self._prune_idempotency()
        now = int(self._clock())
        with self._conn:
            row = self._conn.execute(
                "SELECT result_code, result_digest, created_ms "
                "FROM idempotency WHERE request_id=? AND tool_call_id=?",
                (request_id, tool_call_id)).fetchone()
            if row:
                return ("duplicate", {"result_code": row[0],
                                      "result_digest": row[1],
                                      "created_ms": row[2]})
            self._conn.execute(
                "INSERT INTO idempotency(request_id, tool_call_id,"
                "args_hash, result_code, result_digest, created_ms)"
                " VALUES (?,?,?,?,?,?)",
                (request_id, tool_call_id, args_hash, result_code,
                 result_digest, now))
        return ("stored", {})

    def lookup_idempotent(self, request_id: str,
                          tool_call_id: str) -> Optional[dict]:
        row = self._conn.execute(
            "SELECT result_code, result_digest, created_ms FROM idempotency "
            "WHERE request_id=? AND tool_call_id=?",
            (request_id, tool_call_id)).fetchone()
        if not row:
            return None
        return {"result_code": row[0], "result_digest": row[1],
                "created_ms": row[2]}

    def _prune_idempotency(self) -> None:
        ttl_cut = int(self._clock()) - IDEM_TTL_MS
        with self._conn:
            self._conn.execute(
                "DELETE FROM idempotency WHERE created_ms < ?", (ttl_cut,))
            self._conn.execute(
                "DELETE FROM idempotency WHERE rowid NOT IN ("
                "SELECT rowid FROM idempotency ORDER BY rowid DESC "
                "LIMIT ?)", (IDEM_MAX,))

    def idempotency_count(self) -> int:
        return self._conn.execute(
            "SELECT COUNT(*) FROM idempotency").fetchone()[0]
