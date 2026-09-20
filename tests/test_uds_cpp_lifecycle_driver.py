#!/usr/bin/env python3
"""tests/test_uds_cpp_lifecycle_driver.py — R7_R2 任务 A: C++ UDS 生命周期驱动器。

在 Linux 上运行真实 test_uds_cpp_roundtrip 二进制并做精确断言:
  - 正常 roundtrip: exit=0 + 握手/响应 marker + [child] wait + 零残留;
  - after_tmp:   exit=20 + marker + 零残留 (child=none);
  - after_spawn: exit=21 + marker + 对应 child 不存在 + 零残留;
  - 无效 agent 路径: exit=10 + "agent 目录无效" marker (任何平台可跑);
  - 三种 cwd (build 目录 / 项目根 / /tmp) 全部 exit=0 零残留;
  - 并发两实例: 临时资源互不冲突, 结束后两者目录均不存在。

驱动器不替代业务 roundtrip (业务断言仍在二进制内 check()),
只负责验收进程退出码、输出 marker 与文件/目录收尾。
非 Linux 平台只跑无效 agent 路径场景, 其余场景 SKIP。
"""
import glob
import os
import re
import subprocess
import sys

LINUX = sys.platform.startswith("linux")

g_ran = 0
g_skipped = 0
g_passed = 0
g_failed = 0


def tmp_snapshot():
    """本轮测试的残留观察窗口: /tmp 下 holopet_uds_cpp_* 条目集合。"""
    return set(glob.glob("/tmp/holopet_uds_cpp_*")) if LINUX else set()


def record(name, ok, detail):
    global g_passed, g_failed
    if ok:
        g_passed += 1
        print("  PASS: %s" % name)
    else:
        g_failed += 1
        print("  FAIL: %s — %s" % (name, detail))


def run_binary(binary, cwd=None, env_extra=None, timeout=120):
    """运行二进制; 返回 (rc, stdout文本, 新增 /tmp 条目, socket 路径列表)。"""
    env = dict(os.environ)
    env.pop("HOLOPET_TEST_FAULT", None)
    env.pop("HOLOPET_TEST_AGENT_DIR", None)
    if env_extra:
        env.update(env_extra)
    pre = tmp_snapshot()
    proc = subprocess.Popen([binary], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out, _ = proc.communicate(timeout=timeout)
    text = out.decode("utf-8", "replace")
    new = tmp_snapshot() - pre
    sockets = re.findall(r"^socket: (.+)$", text, re.M)
    return proc.returncode, text, new, sockets


def assert_residue_line(text):
    m = re.search(r"^\[residue\] (.*)$", text, re.M)
    assert m, "缺少 [residue] 行"
    return dict(kv.split("=", 1) for kv in m.group(1).split())


def assert_sockets_gone(sockets):
    for s in sockets:
        assert not os.path.exists(s), "socket 文件残留: %s" % s
        d = os.path.dirname(s)
        assert not os.path.exists(d), "临时目录残留: %s" % d


def assert_no_new_tmp(new):
    assert new == set(), "出现新的 /tmp 残留: %s" % sorted(new)


def scenario_normal(binary, cwd):
    rc, text, new, sockets = run_binary(binary, cwd=cwd)
    assert rc == 0, "exit=%d (期望 0)" % rc
    assert "PASS: UDS 握手 hello 完成" in text, "缺握手 marker"
    assert "PASS: UDS 收到 done" in text, "缺 done marker"
    assert "PASS: agentd 已 wait 并获得真实退出状态" in text, "缺 wait marker"
    assert re.search(r"\[child\] pid=\d+ wait=", text), "缺 [child] wait 行"
    fields = assert_residue_line(text)
    for k in ("tmpdir", "socket", "log", "pid", "status"):
        assert fields[k] == "gone", "残留项 %s=%s" % (k, fields[k])
    assert fields["child"] == "gone", "child=%s" % fields["child"]
    assert_no_new_tmp(new)
    assert_sockets_gone(sockets)


def scenario_after_tmp(binary, cwd):
    rc, text, new, sockets = run_binary(
        binary, cwd=cwd, env_extra={"HOLOPET_TEST_FAULT": "after_tmp"})
    assert rc == 20, "exit=%d (期望 20)" % rc
    assert "[fault] after_tmp:" in text, "缺 after_tmp marker"
    fields = assert_residue_line(text)
    for k in ("tmpdir", "socket", "log", "pid", "status"):
        assert fields[k] == "gone", "残留项 %s=%s" % (k, fields[k])
    assert fields["child"] == "none", "未 spawn 时 child 应为 none"
    assert_no_new_tmp(new)
    assert_sockets_gone(sockets)


def scenario_after_spawn(binary, cwd):
    rc, text, new, sockets = run_binary(
        binary, cwd=cwd, env_extra={"HOLOPET_TEST_FAULT": "after_spawn"})
    assert rc == 21, "exit=%d (期望 21)" % rc
    m = re.search(r"\[fault\] after_spawn: 子进程已启动 pid=(\d+)", text)
    assert m, "缺 after_spawn pid marker"
    pid = int(m.group(1))
    try:
        os.kill(pid, 0)
        alive = True
    except ProcessLookupError:
        alive = False
    assert not alive, "after_spawn 对应子进程 %d 仍存在" % pid
    fields = assert_residue_line(text)
    for k in ("tmpdir", "socket", "log", "pid", "status"):
        assert fields[k] == "gone", "残留项 %s=%s" % (k, fields[k])
    assert fields["child"] == "gone", "child=%s" % fields["child"]
    assert_no_new_tmp(new)
    assert_sockets_gone(sockets)


def scenario_invalid_agent_dir(binary, cwd):
    rc, text, new, sockets = run_binary(
        binary, cwd=cwd,
        env_extra={"HOLOPET_TEST_AGENT_DIR": "/definitely/missing"})
    assert rc == 10, "exit=%d (期望 10)" % rc
    assert "agent 目录无效" in text, "缺 'agent 目录无效' marker"
    assert "[negative]" in text, "缺 [negative] 前缀"


def scenario_concurrent(binary, cwd):
    env = dict(os.environ)
    env.pop("HOLOPET_TEST_FAULT", None)
    env.pop("HOLOPET_TEST_AGENT_DIR", None)
    pre = tmp_snapshot()
    procs = [subprocess.Popen([binary], cwd=cwd, env=env,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
             for _ in range(2)]
    outs = []
    for p in procs:
        out, _ = p.communicate(timeout=120)
        outs.append((p.returncode, out.decode("utf-8", "replace")))
    new = tmp_snapshot() - pre
    for i, (rc, text) in enumerate(outs):
        assert rc == 0, "实例 %d exit=%d (期望 0)" % (i, rc)
        fields = assert_residue_line(text)
        assert fields["child"] == "gone", "实例 %d child=%s" % (i, fields["child"])
        sockets = re.findall(r"^socket: (.+)$", text, re.M)
        assert_sockets_gone(sockets)
    assert_no_new_tmp(new)


def main():
    global g_ran, g_skipped
    argv = sys.argv[1:]
    if not argv:
        print("用法: test_uds_cpp_lifecycle_driver.py <binary> [--project-root ROOT]")
        return 2
    binary = argv[0]
    project_root = None
    if "--project-root" in argv:
        project_root = argv[argv.index("--project-root") + 1]
    print("=== test_uds_cpp_lifecycle_driver (R7_R2 任务 A) ===")
    print("  binary: %s" % binary)
    print("  platform: %s" % sys.platform)
    cwd_build = os.getcwd()
    print("  cwd(build): %s" % cwd_build)

    scenarios = [
        ("invalid-agent-dir (exit=10)",
         lambda: scenario_invalid_agent_dir(binary, cwd_build)),
    ]
    if LINUX:
        scenarios += [
            ("normal roundtrip (cwd=build)",
             lambda: scenario_normal(binary, cwd_build)),
            ("normal roundtrip (cwd=project-root)",
             lambda: scenario_normal(binary, project_root or cwd_build)),
            ("normal roundtrip (cwd=/tmp)",
             lambda: scenario_normal(binary, "/tmp")),
            ("after_tmp (exit=20)", lambda: scenario_after_tmp(binary, cwd_build)),
            ("after_spawn (exit=21)", lambda: scenario_after_spawn(binary, cwd_build)),
            ("concurrent-x2", lambda: scenario_concurrent(binary, cwd_build)),
        ]
    else:
        g_skipped += 6
        print("  SKIP: 非 Linux 平台, AF_UNIX 场景 (normal×3/after_tmp/"
              "after_spawn/concurrent) 跳过, 只跑 agent-dir 无效场景")

    for name, fn in scenarios:
        g_ran += 1
        try:
            fn()
            record(name, True, "")
        except AssertionError as e:
            record(name, False, str(e))
        except Exception as e:   # 基础设施失败也算 FAIL, 不吞
            record(name, False, "exception: %r" % (e,))

    print("\n=== 驱动器结果: ran=%d skipped=%d passed=%d failed=%d ==="
          % (g_ran, g_skipped, g_passed, g_failed))
    return 0 if g_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
