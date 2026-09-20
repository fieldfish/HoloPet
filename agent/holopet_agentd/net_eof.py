"""net_eof.py — 连接关闭判定 (V6-R1, 缺陷 B)

R3-R1: Linux 上对端正常关闭时 recv() 返回 b'' (clean EOF);
Windows 上常以 ConnectionResetError 表现。两者都算"连接已关闭"。
socket.timeout 不算关闭 (服务器未关, 不得误判成功)。
e2e_verify.py 与单元测试共用本实现, 消除平台偶然行为。
"""

import socket


def wait_eof(sock, timeout: float = 2.0):
    """阻塞等待连接关闭。

    返回 (seen: bool, kind: str):
      seen=True  — 连接已关闭 (clean-eof / connection-reset / os-error)
      seen=False — 超时未关闭 (timeout) 或未知状态
    """
    sock.settimeout(timeout)
    try:
        while True:
            d = sock.recv(4096)
            if d == b"":
                return True, "clean-eof"
    except ConnectionResetError:
        return True, "connection-reset"
    except socket.timeout:
        return False, "timeout"
    except OSError:
        return True, "os-error"
