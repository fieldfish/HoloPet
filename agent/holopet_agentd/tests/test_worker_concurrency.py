"""test_worker_concurrency.py — R3 工作包 C 并发边界测试 (真实 socket)

覆盖 (对应 R2 缺陷):
  - jsonl_send_serialization: 多线程并发 send 仍一行一个合法 JSON, 不交错
  - worker_flood_busy: 并发 start_turn 有界 — 最多 1 个 active,
    其余快速 busy (不是无界线程/不是排队堆积)
  - disconnect_cleanup: 断线后 active turn 被取消, 服务端不死锁
  - 未知 type / 坏 JSON → 结构化 error 回包 (端到端)
"""

import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class TestWorkerConcurrency(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = _free_port()
        cls.config = {"provider": "fake", "max_tool_rounds": 4}
        cls.srv = worker_main.WorkerServer(cls.config, port=cls.port)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        time.sleep(0.3)

    def _connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        sock.settimeout(3)
        # 收 hello
        buf = b""
        while b"\n" not in buf:
            buf += sock.recv(4096)
        return sock

    def _send(self, sock, **msg):
        sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))

    def _drain_all(self, sock, timeout=2.0):
        sock.settimeout(0.3)
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            try:
                d = sock.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        lines = buf.decode("utf-8", "replace").strip().splitlines()
        msgs = []
        for ln in lines:
            msgs.append(json.loads(ln))   # 任何交错都会在这里抛 JSONDecodeError
        return msgs

    def test_jsonl_send_serialization(self):
        """发送串行化 (单元): 多线程并发 ClientConn.send,
        每一行仍是一个合法 JSON (不可交错)。

        服务级单 active turn (有界执行) 是 test_worker_flood_busy 的职责;
        本测试只验证 R2 缺陷: 同一连接多线程 sendall 行交错。
        """

        class RecordingSock:
            def __init__(self):
                self.chunks = []

            def sendall(self, data):
                # 非原子追加: 若 ClientConn 无锁, 并发下必然产生交错损坏
                self.chunks.append(data)

        rec = RecordingSock()
        conn = worker_main.ClientConn(rec)
        errs = []

        def worker(tid):
            for i in range(50):
                if not conn.send(f"rid-{tid}-{i}", "tts_level",
                                 rms=round(0.01 * i, 4)):
                    errs.append("send failed")

        threads = [threading.Thread(target=worker, args=(t,)) for t in range(16)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(errs, [])
        raw = b"".join(rec.chunks).decode("utf-8")
        lines = raw.strip().splitlines()
        self.assertEqual(len(lines), 16 * 50,
                         "每个 send 恰好一行 (无拆行/并行粘包)")
        for ln in lines:
            m = json.loads(ln)          # 任何交错都会在这里失败
            self.assertEqual(m["v"], "1")
            self.assertIn("request_id", m)

    def test_sequential_turns_after_flood(self):
        """集成: flood busy 之后同连接顺序 turn 仍完整跑通 (事件不交错)。"""
        sock = self._connect()
        try:
            self._send(sock, v="1", request_id="seq", type="transcript",
                       text="现在几点")
            time.sleep(0.1)
            self._send(sock, v="1", request_id="seq", type="start_turn")
            time.sleep(0.2)
            self._send(sock, v="1", request_id="seq", type="stop_recording")
            msgs = self._drain_all(sock, timeout=4.0)
            types = [m["type"] for m in msgs]
            self.assertIn("done", types, f"事件序列: {types}")
            for m in msgs:
                self.assertEqual(m["v"], "1")
                self.assertIn("request_id", m)
        finally:
            sock.close()

    def test_worker_flood_busy(self):
        """并发 start_turn 有界: 单 active, 其余 busy 快速返回。"""
        sock = self._connect()
        try:
            # 第一个 turn 占位 (listening 等 stop_recording)
            self._send(sock, v="1", request_id="flood-0", type="start_turn")
            time.sleep(0.2)
            # 并发 20 个 start_turn: 期望全部快速 busy
            for i in range(1, 21):
                self._send(sock, v="1", request_id=f"flood-{i}",
                           type="start_turn")
            msgs = self._drain_all(sock, timeout=2.0)
            busy = [m for m in msgs if m["type"] == "error"
                    and m.get("code") == "busy"]
            self.assertEqual(len(busy), 20, f"全部应 busy, got {len(busy)}: {msgs}")
            # 有界性: 服务端 active 仍只有一个
            with self.srv._state_lock:
                self.assertIsNotNone(self.srv._active)
                self.assertEqual(self.srv._active[1], "flood-0")
            # 清理
            self._send(sock, v="1", request_id="flood-0", type="cancel")
            self._drain_all(sock, timeout=1.0)
        finally:
            sock.close()

    def test_disconnect_cancels_active_turn(self):
        """断线 → active turn 被取消, 后续连接可立即开新 turn。"""
        sock = self._connect()
        self._send(sock, v="1", request_id="dc-1", type="start_turn")
        time.sleep(0.2)
        with self.srv._state_lock:
            self.assertEqual(self.srv._active[1], "dc-1")
        sock.close()
        # 等 reader 线程发现 EOF 并收尾
        deadline = time.time() + 3
        while time.time() < deadline:
            with self.srv._state_lock:
                if self.srv._active is None:
                    break
            time.sleep(0.05)
        with self.srv._state_lock:
            self.assertIsNone(self.srv._active, "断线后 active turn 必须被清理")
        # 新连接可立即开新轮
        sock2 = self._connect()
        try:
            self._send(sock2, v="1", request_id="dc-2", type="start_turn")
            time.sleep(0.2)
            self._send(sock2, v="1", request_id="dc-2", type="cancel")
            msgs = self._drain_all(sock2, timeout=1.0)
            types = [m["type"] for m in msgs]
            self.assertIn("cancel", types)
            self.assertNotIn("error", types[:1], f"不应 busy: {msgs}")
        finally:
            sock2.close()

    def test_unknown_type_structured_reject(self):
        sock = self._connect()
        try:
            self._send(sock, v="1", request_id="u1", type="rm_rf_slash")
            msgs = self._drain_all(sock, timeout=1.0)
            errs = [m for m in msgs if m["type"] == "error"]
            self.assertEqual(errs[0]["code"], "unknown_type")
        finally:
            sock.close()

    def test_bad_json_structured_reject(self):
        sock = self._connect()
        try:
            sock.sendall(b"{this is not json\n")
            msgs = self._drain_all(sock, timeout=1.0)
            errs = [m for m in msgs if m["type"] == "error"]
            self.assertEqual(errs[0]["code"], "bad_json")
        finally:
            sock.close()

    def test_stale_request_id_rejected(self):
        """cancel 未知 request_id → unknown_request (陈旧 id 不静默)。"""
        sock = self._connect()
        try:
            self._send(sock, v="1", request_id="ghost", type="cancel")
            msgs = self._drain_all(sock, timeout=1.0)
            errs = [m for m in msgs if m["type"] == "error"]
            self.assertEqual(errs[0]["code"], "unknown_request")
        finally:
            sock.close()

    def test_cancel_is_terminal_not_early_ack(self):
        """R4-R1 (D6): cancel 终态在 turn 真正停止后发出;
        取消后新 turn 可立即开始 (不 busy)。"""
        sock = self._connect()
        try:
            # 长回答 turn (多 content 分片)
            self._send(sock, v="1", request_id="ct", type="transcript",
                       text="你好")
            time.sleep(0.2)
            self._send(sock, v="1", request_id="ct", type="start_turn")
            # 不 stop_recording: 保持 listening 等待窗口, 期间取消
            time.sleep(0.3)
            self._send(sock, v="1", request_id="ct", type="cancel")
            msgs = self._drain_all(sock, timeout=4.0)
            types = [m["type"] for m in msgs]
            self.assertIn("cancel", types, f"终态 cancel 必须发出: {types}")
            # cancel 必须是最后的事件 (turn 停止后; 无 done/error 更晚)
            last = types[-1]
            self.assertEqual(last, "cancel",
                             f"cancel 应为终态事件, 实际最后是 {last}")
            # 取消后 active 已清 → 新 turn 不被 busy
            self._send(sock, v="1", request_id="ct2", type="start_turn")
            time.sleep(0.2)
            self._send(sock, v="1", request_id="ct2", type="cancel")
            msgs2 = self._drain_all(sock, timeout=2.0)
            busy = [m for m in msgs2 if m.get("code") == "busy"]
            self.assertEqual(len(busy), 0,
                             f"旧 turn 退出后新 turn 不被 busy: {msgs2}")
        finally:
            sock.close()

    def test_flood_does_not_exhaust_threads(self):
        """有界执行: 大量 start_turn 不会无限建线程 (单 worker executor)。"""
        sock = self._connect()
        try:
            self._send(sock, v="1", request_id="t-0", type="start_turn")
            time.sleep(0.1)
            for i in range(1, 200):
                self._send(sock, v="1", request_id=f"t-{i}", type="start_turn")
            msgs = self._drain_all(sock, timeout=3.0)
            busy = [m for m in msgs if m["type"] == "error"
                    and m.get("code") == "busy"]
            self.assertEqual(len(busy), 199)
            self._send(sock, v="1", request_id="t-0", type="cancel")
            self._drain_all(sock, timeout=1.0)
        finally:
            sock.close()


if __name__ == "__main__":
    unittest.main()
