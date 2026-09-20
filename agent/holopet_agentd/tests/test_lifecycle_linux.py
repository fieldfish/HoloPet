"""test_lifecycle_linux.py — R4 P2: agentd 生命周期 (Linux UDS 与可移植部分)

Linux (AF_UNIX) 5 项真实运行场景:
  hello / 半包 / 粘包 / clean EOF / 陈旧 socket / 重启 / 旧 request_id
Windows 无 AF_UNIX → UDS 部分 SKIP (可移植的 fail-closed 与 executor
rollback 部分在两个平台都运行)。

全部使用临时目录 socket, 不写死 /run/holopet/agent.sock。

R7 修复:
  A5: JsonlClient 持久接收缓冲 (粘包不丢帧)。
  A6: readiness 以真实 connect 成功为准。
  A7: speaking 断言改为既有协议 state+value=speaking。
  A8: 重启测试先停第一实例并 wait; 客户端 socket 在 finally 关闭。
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd import main as worker_main  # noqa: E402

HAVE_AF_UNIX = hasattr(socket, "AF_UNIX")
CONFIG = {"provider": "fake", "max_tool_rounds": 4}


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


class TestLifecyclePortable(unittest.TestCase):
    """无平台依赖的生命周期语义 (Windows + Linux 均运行)。"""

    def test_uds_path_fail_closed_on_regular_file(self):
        if not HAVE_AF_UNIX:
            self.skipTest("AF_UNIX not available")
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "agent.sock")
            with open(p, "w", encoding="utf-8") as f:
                f.write("not a socket")
            srv = worker_main.WorkerServer(CONFIG, uds_path=p)
            with self.assertRaises(OSError):
                srv._prepare_uds_path()      # 普通文件 → fail closed

    def test_uds_path_fail_closed_on_symlink(self):
        if not HAVE_AF_UNIX:
            self.skipTest("AF_UNIX not available")
        with tempfile.TemporaryDirectory() as td:
            target = os.path.join(td, "victim.txt")
            with open(target, "w", encoding="utf-8") as f:
                f.write("data")
            p = os.path.join(td, "agent.sock")
            os.symlink(target, p)            # 符号链接 → 不允许 unlink
            srv = worker_main.WorkerServer(CONFIG, uds_path=p)
            with self.assertRaises(OSError):
                srv._prepare_uds_path()

    def test_executor_submit_rollback(self):
        """executor 已关闭时 start_turn → 回滚 _active, 返回 internal。"""
        srv = worker_main.WorkerServer(CONFIG, port=0)
        srv._turn_worker.shutdown(timeout=1.0)   # R4-R1: 有界 worker
        client = worker_main.ClientConn(None)
        class _Sock:
            def __init__(self):
                self.calls = []
            def sendall(self, data):
                self.calls.append(data)
        sock = _Sock()
        client.sock = sock
        msg = {"v": "1", "request_id": "r-rollback", "type": "start_turn"}
        srv._handle_line(client, '{"v":"1","request_id":"r-rollback",'
                                 '"type":"start_turn"}')
        with srv._state_lock:
            self.assertIsNone(srv._active, "submit 失败必须回滚 active")
            self.assertNotIn("r-rollback", srv._cancel_flags)
        sent = b"".join(sock.calls).decode("utf-8")
        self.assertIn('"code": "internal"', sent)

    def test_shutdown_idempotent(self):
        """重复 shutdown 安全; 未启动服务的 shutdown 也安全。"""
        srv = worker_main.WorkerServer(CONFIG, port=0)
        srv.shutdown()
        srv.shutdown()          # 幂等
        self.assertTrue(srv._shutdown_done)


@unittest.skipUnless(HAVE_AF_UNIX, "AF_UNIX not available (Windows dev uses "
                                   "loopback TCP); Linux/CI MUST PASS here")
class TestLifecycleLinux(unittest.TestCase):
    """Linux UDS 生命周期 5 项真实运行 (R4-G5)。"""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="holopet_life_")
        self.sock_path = os.path.join(self.tmpdir, "agent.sock")
        self._clients = []
        self.srv = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        self.thread = threading.Thread(target=self.srv.serve_forever,
                                       daemon=True)
        self.thread.start()
        # R7 (A6): readiness = 真实 connect 成功
        deadline = time.time() + 5
        ready = False
        while time.time() < deadline:
            if not self.thread.is_alive():
                raise RuntimeError("server 线程提前退出")
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            probe.settimeout(0.3)
            try:
                probe.connect(self.sock_path)
                probe.close()
                ready = True
                break
            except OSError:
                probe.close()
                time.sleep(0.03)
        if not ready:
            raise RuntimeError(f"server readiness 超时: {self.sock_path}")

    def tearDown(self):
        for c in self._clients:
            c.close()
        try:
            self.srv.shutdown(timeout=2.0)
        except OSError:
            pass
        self.thread.join(timeout=5)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _connect(self):
        c = JsonlClient(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        c.sock.settimeout(5)
        c.sock.connect(self.sock_path)
        self._clients.append(c)
        return c

    def test_hello_and_clean_eof(self):
        c = self._connect()
        hello = c.recv_line()
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["v"], "1")
        # clean EOF: 客户端正常关闭 → server 存活
        c.shutdown_write()
        c.close()
        time.sleep(0.3)
        c2 = self._connect()
        self.assertEqual(c2.recv_line()["type"], "hello",
                         "clean EOF 后 server 必须存活")
        c2.close()

    def test_partial_and_sticky_lines(self):
        c = self._connect()
        c.recv_line()   # hello
        raw = json.dumps({"v": "1", "request_id": "p1", "type": "cancel"}).encode()
        c.sock.sendall(raw[:5])                       # 半包
        time.sleep(0.1)
        c.sock.sendall(raw[5:] + b"\n")
        self.assertEqual(c.recv_line()["code"], "unknown_request")
        l1 = json.dumps({"v": "1", "request_id": "p2", "type": "cancel"})
        l2 = json.dumps({"v": "1", "request_id": "p3", "type": "bogus"})
        c.sock.sendall((l1 + "\n" + l2 + "\n").encode())   # 粘包
        self.assertEqual(c.recv_line()["code"], "unknown_request")
        self.assertEqual(c.recv_line()["code"], "unknown_type")
        c.close()

    def test_stale_socket_and_restart(self):
        """R7 (A8): 先正常停止第一实例并 wait (socket 被删除),
        再构造真实 stale socket 重启 — 场景分离。"""
        self.srv.shutdown(timeout=2.0)
        self.thread.join(timeout=5)
        self.assertFalse(os.path.exists(self.sock_path),
                         "正常退出必须删除自建 socket")
        # 真实 stale: bind 后从未 listen
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(self.sock_path)
        stale.close()
        srv2 = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        t2 = threading.Thread(target=srv2.serve_forever, daemon=True)
        t2.start()
        deadline = time.time() + 5
        ready = False
        while time.time() < deadline:
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            probe.settimeout(0.3)
            try:
                probe.connect(self.sock_path)
                probe.close()
                ready = True
                break
            except OSError:
                probe.close()
                time.sleep(0.03)
        self.assertTrue(ready, "重启后 readiness 必须成功")
        c = self._connect()
        self.assertEqual(c.recv_line()["type"], "hello", "重启后 hello")
        c.close()
        srv2.shutdown(timeout=2.0)
        t2.join(timeout=5)
        self.assertFalse(os.path.exists(self.sock_path),
                         "服务结束后必须删除自建 socket 文件")

    def test_old_request_id_rejected_after_turn(self):
        c = self._connect()
        c.recv_line()
        c.send(v="1", request_id="old", type="transcript", text="你好")
        time.sleep(0.2)
        c.send(v="1", request_id="old", type="start_turn")
        time.sleep(0.2)
        c.send(v="1", request_id="old", type="stop_recording")
        # 等 done
        c.sock.settimeout(0.3)
        buf = b""
        end = time.time() + 4
        while time.time() < end:
            try:
                d = c.sock.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        types = [json.loads(ln)["type"]
                 for ln in buf.decode("utf-8", "replace").strip().splitlines()]
        self.assertIn("done", types)
        # 陈旧 id cancel → unknown_request (不污染新轮)
        c.send(v="1", request_id="old", type="cancel")
        self.assertEqual(c.recv_line()["code"], "unknown_request")
        c.close()

    def test_full_turn_with_content_flow(self):
        """UDS 上跑通 R4 完整状态流 (含 content/response_complete)。"""
        c = self._connect()
        c.recv_line()
        c.send(v="1", request_id="t", type="transcript", text="你好")
        time.sleep(0.2)
        c.send(v="1", request_id="t", type="start_turn")
        time.sleep(0.2)
        c.send(v="1", request_id="t", type="stop_recording")
        c.sock.settimeout(0.3)
        buf = b""
        end = time.time() + 4
        while time.time() < end:
            try:
                d = c.sock.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
        evs = [json.loads(ln) for ln in
               buf.decode("utf-8", "replace").strip().splitlines()]
        types = [e["type"] for e in evs]
        for need in ("content", "response_complete", "expression",
                     "tts_finished", "done"):
            self.assertIn(need, types, f"事件序列缺 {need}: {types}")
        # R7 (A7): 生产协议是 state 事件 + value=speaking, 不是 type=speaking
        speaking = [e for e in evs
                    if e["type"] == "state" and e.get("value") == "speaking"]
        self.assertTrue(speaking,
                        f"缺 state+value=speaking 事件: {types}")


@unittest.skipUnless(HAVE_AF_UNIX, "AF_UNIX 不可用 (Windows 开发环境): "
                     "SIGTERM 子进程清理测试仅 Linux 运行")
class TestAgentdSigtermLinux(unittest.TestCase):
    """R8_R1 (A4 回归): agentd 收到 SIGTERM 后在有界时间内优雅退出,
    且 UDS socket 被删除 (现场 systemd/脚本停止路径)。

    使用真实子进程与真实信号 (不是 in-process shutdown 调用):
      spawn python -m holopet_agentd --uds <tmp> --fake
      -> 等 socket 出现 -> SIGTERM -> deadline 内退出 + socket 消失。
    """

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="holopet_sigterm_")
        self.sock_path = os.path.join(self.tmpdir, "agent.sock")
        # 项目 agent/ 目录 (PYTHONPATH 需其父含 holopet_agentd 包; config 在项目根/config)
        agent_dir = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        env = dict(os.environ)
        env["PYTHONPATH"] = agent_dir
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "holopet_agentd",
             "--config", os.path.join(agent_dir, "..", "config",
                                      "agent.example.json"),
             "--uds", self.sock_path, "--fake", "--log-level", "WARNING"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # readiness: 等 socket 文件出现且可连接
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline:
            if os.path.exists(self.sock_path):
                try:
                    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    probe.settimeout(0.5)
                    probe.connect(self.sock_path)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    pass
            time.sleep(0.05)
        if not ready:
            self.proc.kill()
            self.proc.wait(timeout=5)
            raise RuntimeError("agentd 子进程 readiness 超时")

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_sigterm_graceful_exit_and_socket_removed(self):
        self.proc.send_signal(signal.SIGTERM)
        t0 = time.monotonic()
        try:
            rc = self.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.fail("SIGTERM 后 5s 内未退出 (未走统一 shutdown 路径)")
        elapsed = time.monotonic() - t0
        self.assertEqual(rc, 0,
                         f"SIGTERM 路径应优雅退出 rc=0, 实际 {rc}")
        self.assertLess(elapsed, 5.0)
        self.assertFalse(os.path.exists(self.sock_path),
                         "SIGTERM 优雅退出必须删除自建 UDS socket")

    def test_sigterm_then_restart_no_residue(self):
        """SIGTERM 退出后立刻重启: 无 stale socket 冲突、无孤儿进程。"""
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(timeout=5.0)
        # 项目 agent/ 目录 (PYTHONPATH 需其父含 holopet_agentd 包; config 在项目根/config)
        agent_dir = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        env = dict(os.environ)
        env["PYTHONPATH"] = agent_dir
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        proc2 = subprocess.Popen(
            [sys.executable, "-m", "holopet_agentd",
             "--uds", self.sock_path, "--fake", "--log-level", "WARNING"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            ok = False
            while time.monotonic() < deadline:
                if os.path.exists(self.sock_path):
                    try:
                        probe = socket.socket(socket.AF_UNIX,
                                              socket.SOCK_STREAM)
                        probe.settimeout(0.5)
                        probe.connect(self.sock_path)
                        probe.close()
                        ok = True
                        break
                    except OSError:
                        pass
                time.sleep(0.05)
            self.assertTrue(ok, "SIGTERM 后重启应立即就绪 (无残留阻塞)")
        finally:
            if proc2.poll() is None:
                proc2.send_signal(signal.SIGTERM)
                try:
                    proc2.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    proc2.kill()
                    proc2.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
