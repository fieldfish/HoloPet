"""test_eof_semantics.py — R3-R1 缺陷 B: clean EOF / reset / timeout 可重复单测

不依赖平台偶然行为:
  - clean EOF: 服务端 shutdown+close → 客户端必须判定"已关闭"
  - reset: SO_LINGER(0) close → 客户端必须判定"已关闭"
  - timeout: 服务端保持连接 → 客户端必须判定"未关闭" (不得误判成功)
"""

import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from holopet_agentd.net_eof import wait_eof  # noqa: E402


def _start_listener():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    return srv, port


class TestEofSemantics(unittest.TestCase):
    def test_clean_eof(self):
        srv, port = _start_listener()
        conn_holder = {}

        def server():
            c, _ = srv.accept()
            conn_holder["conn"] = c
            c.shutdown(socket.SHUT_WR)   # 干净关闭 → 对端 recv 得 b''
            c.close()

        threading.Thread(target=server, daemon=True).start()
        client = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            time.sleep(0.3)
            seen, kind = wait_eof(client, timeout=2.0)
            self.assertTrue(seen, f"clean EOF 必须判定为已关闭, got {kind}")
            self.assertEqual(kind, "clean-eof")
        finally:
            client.close()
            srv.close()

    def test_reset_eof(self):
        srv, port = _start_listener()

        def server():
            c, _ = srv.accept()
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                         __import__("struct").pack("ii", 1, 0))  # RST 关闭
            c.close()

        threading.Thread(target=server, daemon=True).start()
        client = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            time.sleep(0.3)
            # 触发 RST 到达: 发一点数据再读 (RST 可能先到 → sendall 抛错也算关闭)
            try:
                client.sendall(b"ping")
            except ConnectionResetError:
                pass
            seen, kind = wait_eof(client, timeout=2.0)
            self.assertTrue(seen, f"reset 必须判定为已关闭, got {kind}")
            self.assertIn(kind, ("connection-reset", "clean-eof", "os-error"))
        finally:
            client.close()
            srv.close()

    def test_timeout_is_not_eof(self):
        srv, port = _start_listener()

        def server():
            c, _ = srv.accept()
            conn_holder.append(c)   # 保持连接打开

        conn_holder = []
        threading.Thread(target=server, daemon=True).start()
        client = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            time.sleep(0.3)
            seen, kind = wait_eof(client, timeout=0.5)
            self.assertFalse(seen, "服务器未关闭时不得判定 EOF (超时≠成功)")
            self.assertEqual(kind, "timeout")
        finally:
            client.close()
            for c in conn_holder:
                c.close()
            srv.close()


if __name__ == "__main__":
    unittest.main()
