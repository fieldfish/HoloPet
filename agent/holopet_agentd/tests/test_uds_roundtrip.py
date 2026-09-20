"""test_uds_roundtrip.py — R3-R1 缺陷 D1: Unix Domain Socket 真实 roundtrip

不需要 Raspberry Pi 硬件, 普通 Linux 软件环境即可 (R1-G9)。
覆盖: 启动 / hello / 半包 / 粘包 / 正常 EOF / 陈旧 socket 文件安全处理 /
关闭后重启 / 旧 request_id 拒绝。
socket 路径用临时目录, 不写死 /run/holopet/agent.sock。
Windows 无 AF_UNIX → SKIP (Linux/CI 必须 PASS)。

R7 修复:
  A5: JsonlClient 持久接收缓冲 — 粘包多行不再丢弃余量。
  A6: readiness 以真实 connect 成功为准, 不只看路径存在。
  A8: stale socket 测试构造真实 stale 文件; finally 关闭客户端与 wait 线程。
"""

import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402

HAVE_AF_UNIX = hasattr(socket, "AF_UNIX")


class JsonlClient:
    """R7 (A5): 连接级 JSONL 客户端 — recv 缓冲跨调用保留, 粘包不丢帧。"""

    def __init__(self, sock):
        self.sock = sock
        self._buf = b""

    def send(self, **msg):
        self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))

    def recv_line(self, timeout=5.0):
        self.sock.settimeout(timeout)
        while b"\n" not in self._buf:
            d = self.sock.recv(4096)
            if not d:
                break
            self._buf += d
        if b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            return json.loads(line.decode("utf-8"))
        # 连接关闭前最后一行 (无尾随换行)
        if self._buf:
            line, self._buf = self._buf, b""
            return json.loads(line.decode("utf-8"))
        raise ConnectionError("连接关闭且无数据")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def shutdown_write(self):
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass


@unittest.skipUnless(HAVE_AF_UNIX, "AF_UNIX 不可用 (Windows 开发回退用 TCP); "
                                   "Linux/CI 必须 PASS 本测试")
class TestUdsRoundtrip(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="holopet_uds_test_")
        self.sock_path = os.path.join(self.tmpdir, "agent.sock")
        self.config = {"provider": "fake", "max_tool_rounds": 4}
        self._clients = []
        self._servers = []            # (srv, thread) 全部登记, tearDown 清理
        self.srv, self._srv_thread = self._start_server(self.sock_path)
        self._servers.append((self.srv, self._srv_thread))

    def tearDown(self):
        for c in self._clients:
            c.close()
        for srv, t in self._servers:
            try:
                srv.shutdown(timeout=2.0)
            except OSError:
                pass
            t.join(timeout=5)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _start_server(self, path):
        srv = worker_main.WorkerServer(self.config, uds_path=path)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        # R7 (A6): readiness = 真实 connect 成功 (stale 文件存在时路径早已
        # 存在, 不能只看 os.path.exists)
        deadline = time.time() + 5
        while time.time() < deadline:
            if not t.is_alive():
                raise RuntimeError("server 线程提前退出")
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            probe.settimeout(0.3)
            try:
                probe.connect(path)
                probe.close()
                return srv, t
            except OSError:
                probe.close()
                time.sleep(0.03)
        raise RuntimeError(f"server readiness 超时: {path}")

    def _connect(self):
        c = JsonlClient(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        c.sock.settimeout(5)
        c.sock.connect(self.sock_path)
        self._clients.append(c)
        return c

    def test_stale_socket_file_safe_restart(self):
        """真实 stale socket 文件 (无活跃服务) → 新 server 安全 bind 并服务。"""
        # 构造真实 stale: bind 后立即 close, 从未 listen, 且无活跃实例
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(os.path.join(self.tmpdir, "stale.sock"))
        stale.close()
        srv2, t2 = self._start_server(os.path.join(self.tmpdir, "stale.sock"))
        self._servers.append((srv2, t2))
        c = JsonlClient(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        c.sock.settimeout(5)
        c.sock.connect(os.path.join(self.tmpdir, "stale.sock"))
        self._clients.append(c)
        hello = c.recv_line()
        self.assertEqual(hello["type"], "hello")

    def test_hello_then_turn_roundtrip(self):
        c = self._connect()
        hello = c.recv_line()
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["v"], "1")
        # 完整一轮
        c.send(v="1", request_id="u1", type="transcript", text="现在几点")
        time.sleep(0.2)
        c.send(v="1", request_id="u1", type="start_turn")
        time.sleep(0.2)
        c.send(v="1", request_id="u1", type="stop_recording")
        events = self._drain(c, 4.0)
        types = [e["type"] for e in events]
        self.assertIn("done", types, f"事件: {types}")
        # 正常 EOF: 客户端关闭 → recv b'' (服务端 reader 退出不挂)
        c.close()

    def test_partial_and_sticky_lines(self):
        """半包: 一行 JSON 分两次发; 粘包: 两行一次发 (持久缓冲不得丢帧)。"""
        c = self._connect()
        c.recv_line()   # hello
        # 半包
        raw = json.dumps({"v": "1", "request_id": "p1", "type": "cancel"}).encode()
        c.sock.sendall(raw[:5])
        time.sleep(0.1)
        c.sock.sendall(raw[5:] + b"\n")
        m = c.recv_line()
        self.assertEqual(m["type"], "error")    # 未知 request_id → 结构化拒绝
        self.assertEqual(m["code"], "unknown_request")
        # 粘包: cancel(未知 rid) + 未知 type 两行一起
        l1 = json.dumps({"v": "1", "request_id": "p2", "type": "cancel"})
        l2 = json.dumps({"v": "1", "request_id": "p3", "type": "bogus"})
        c.sock.sendall((l1 + "\n" + l2 + "\n").encode())
        m1 = c.recv_line()
        m2 = c.recv_line()
        self.assertEqual(m1["code"], "unknown_request")
        self.assertEqual(m2["code"], "unknown_type")
        c.close()

    def test_clean_eof_server_side_ok(self):
        """客户端正常关闭 (shutdown+close): 服务端 reader 必须退出且不崩溃;
        随后新连接仍可正常服务 (server 未被带崩)。"""
        c = self._connect()
        c.recv_line()
        c.shutdown_write()
        c.close()
        time.sleep(0.3)
        c2 = self._connect()
        hello = c2.recv_line()
        self.assertEqual(hello["type"], "hello", "EOF 后 server 必须存活")
        c2.close()

    def test_old_request_id_rejected_after_turn(self):
        """turn 完成后, 同 request_id 的 cancel → unknown_request (陈旧 id 丢弃)。"""
        c = self._connect()
        c.recv_line()
        c.send(v="1", request_id="old", type="transcript", text="你好")
        time.sleep(0.2)
        c.send(v="1", request_id="old", type="start_turn")
        time.sleep(0.2)
        c.send(v="1", request_id="old", type="stop_recording")
        events = self._drain(c, 4.0)
        self.assertIn("done", [e["type"] for e in events])
        # 轮次已结束 → 陈旧 id 拒绝
        c.send(v="1", request_id="old", type="cancel")
        m = c.recv_line()
        self.assertEqual(m["code"], "unknown_request")
        c.close()

    def _drain(self, c, timeout):
        c.sock.settimeout(0.3)
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            try:
                d = c.sock.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        out = []
        for ln in buf.decode("utf-8", "replace").strip().splitlines():
            out.append(json.loads(ln))
        return out


if __name__ == "__main__":
    unittest.main()
