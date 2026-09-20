"""test_uds_ownership.py — R4-R1 P4 (10.3): UDS 路径所有权 (Linux)

覆盖: stale socket / active socket (第二实例拒绝) / regular file /
directory / symlink / path replacement / restart。
Windows 无 AF_UNIX → 全部 SKIP (Linux/CI 必须实跑)。

R7 修复:
  A6: _start readiness 以真实 connect 成功为准 (stale 文件已存在时
      不能只看 os.path.exists)。
  A8: 每个测试 finally 关闭客户端 socket、shutdown+wait 服务线程;
      所有权(第二实例拒绝)与重启(先停后起)场景分离。
"""

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
CONFIG = {"provider": "fake", "max_tool_rounds": 4}


@unittest.skipUnless(HAVE_AF_UNIX, "AF_UNIX not available (Windows dev uses "
                                   "loopback TCP); Linux/CI MUST PASS here")
class TestUdsOwnership(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="holopet_uds_own_")
        self.sock_path = os.path.join(self.tmpdir, "agent.sock")
        self._running = []        # (srv, thread) 登记, tearDown 统一清理

    def tearDown(self):
        for srv, t in self._running:
            try:
                srv.shutdown(timeout=2.0)
            except OSError:
                pass
            t.join(timeout=5)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _start(self):
        """R7 (A6): 启动并等待 readiness = 真实 connect 成功。"""
        srv = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        self._running.append((srv, t))
        deadline = time.time() + 5
        while time.time() < deadline:
            if not t.is_alive():
                raise RuntimeError("server 线程提前退出")
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            probe.settimeout(0.3)
            try:
                probe.connect(self.sock_path)
                probe.close()
                return srv, t
            except OSError:
                probe.close()
                time.sleep(0.03)
        raise RuntimeError(f"server readiness 超时: {self.sock_path}")

    def test_stale_socket_unlinked_then_bind(self):
        """真实 stale socket (bind 后从未 listen) → 新 server unlink 并 bind。"""
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(self.sock_path)      # 创建但从未 listen → 连接必失败
        stale.close()                    # 留下 stale 文件
        srv, t = self._start()           # 应探测失败 → unlink → bind 成功
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            c.connect(self.sock_path)    # 能连上即成功
        finally:
            c.close()
            srv.shutdown(timeout=2.0)
            t.join(timeout=5)

    def test_active_socket_refuses_second_instance(self):
        """R7 (A8): 第一实例活跃时第二实例必须快速拒绝, 不得夺取 socket。"""
        srv, t = self._start()
        try:
            srv2 = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
            with self.assertRaises(OSError):
                srv2._prepare_uds_path()     # 活跃服务 → 拒绝第二实例
            # 活跃 socket 必须仍归第一实例所有
            self.assertTrue(os.path.exists(self.sock_path))
        finally:
            srv.shutdown(timeout=2.0)
            t.join(timeout=5)

    def test_regular_file_fail_closed(self):
        with open(self.sock_path, "w", encoding="utf-8") as f:
            f.write("x")
        srv = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        with self.assertRaises(OSError):
            srv._prepare_uds_path()

    def test_directory_fail_closed(self):
        os.mkdir(self.sock_path)
        srv = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        with self.assertRaises(OSError):
            srv._prepare_uds_path()

    def test_symlink_fail_closed(self):
        target = os.path.join(self.tmpdir, "victim")
        with open(target, "w", encoding="utf-8") as f:
            f.write("data")
        os.symlink(target, self.sock_path)
        srv = worker_main.WorkerServer(CONFIG, uds_path=self.sock_path)
        with self.assertRaises(OSError):
            srv._prepare_uds_path()

    def test_path_replacement_kept_on_exit(self):
        srv, t = self._start()
        try:
            # 服务运行中把 socket 路径替换成普通文件 → 退出时必须保留并报警
            os.unlink(self.sock_path)
            with open(self.sock_path, "w", encoding="utf-8") as f:
                f.write("replaced")
        finally:
            srv.shutdown(timeout=2.0)
            t.join(timeout=5)
        self.assertTrue(os.path.exists(self.sock_path),
                        "被替换的路径必须保留 (不删除他人文件)")
        self.assertTrue(os.path.isfile(self.sock_path))

    def test_restart_reuses_path(self):
        """R7 (A8): 先正常停止并 wait 第一实例 (socket 被删除),
        再构造真实 stale socket 重启 — 两个目标分离验证。"""
        srv, t = self._start()
        srv.shutdown(timeout=2.0)
        t.join(timeout=5)
        self.assertFalse(os.path.exists(self.sock_path),
                         "正常退出删除自己创建的 socket")
        # 真实 stale: 手动留下一个从未 listen 的 socket 文件
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(self.sock_path)
        stale.close()
        srv2, t2 = self._start()         # 重启成功 (路径可复用)
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            c.connect(self.sock_path)
        finally:
            c.close()
            srv2.shutdown(timeout=2.0)
            t2.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
