#!/usr/bin/env python3
"""check_build_matrix.py — CMake 开关组合矩阵检查  V6-R3

R3 修正 (相对 R2, 只比较退出码的缺陷):
  1. 工具链预检: 跑矩阵前确认 cmake、generator 与 C++ 编译器可用;
     预检失败 → 整轮 FAIL (不再让“没有编译器”伪装成任何结果)。
  2. 预期失败组合必须匹配 CMake guard 的指定诊断文字 —
     任意原因的失败不再计为 PASS。
  3. 成功组合的 configure 若失败也 FAIL, 且区分“依赖缺失(SDL)”与
     “工具链问题”: 工具链问题 → 整轮 FAIL。
  4. 记录 generator / 架构 / CMAKE_PREFIX_PATH / 编译器标识到日志。
  5. Windows: 默认选择已安装的 Visual Studio generator;
     Ninja 仅当 cl/g++ 已在有效编译环境时使用。
  6. 每组合独立临时 build 目录, 退出清理; 源码目录不得残留 CMakeCache。
  7. 工作目录接近 Windows 路径上限时提示复制到短目录 (如 C:\\work\\HoloPet)。

用法: python scripts/check_build_matrix.py [--acceptance] [--allow-skip-ai]
环境: CMAKE_BIN / CMAKE_GENERATOR / SDL_PREFIX (SDL 前缀, 启用 ai_on 组合)

R3-R1 (缺陷 C): 区分验收模式与开发模式。
  --acceptance    验收模式 (默认): ai_on 因缺 SDL 无法执行时整轮 FAIL
                  (非零退出, 打印 INCOMPLETE), 不得输出 PASS。
  --allow-skip-ai 开发模式: 允许跳过 ai_on (打印 SKIP); 该模式结果
                  不得写成 G8 PASS。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE = os.environ.get("CMAKE_BIN", "cmake")
LOG_PATH = ROOT / "logs" / "build_matrix.txt"

# 路径长度提示阈值 (MSBuild FileTracker 曾在深层目录失败, R3 主动检测)
PATH_WARN = 140
PATH_FAIL = 200
SHORT_PATH_HINT = r"C:\work\HoloPet"

# (名称, defines, 期望 rc, 期望诊断文字)
# 预期失败必须匹配顶层 CMakeLists.txt 的 guard 文字 (缺一不可)。
MATRIX = [
    ("core_defaults", [], 0, None),
    ("tests_on", ["BUILD_TESTS=ON"], 0, None),
    ("projection_wo_ai_FATAL", ["BUILD_PROJECTION=ON"], 1,
     "BUILD_PROJECTION requires BUILD_AI=ON"),
    ("audio_wo_ai_FATAL", ["BUILD_AUDIO=ON"], 1,
     "BUILD_AUDIO requires BUILD_AI=ON"),
    ("integrated_wo_display_FATAL", ["BUILD_INTEGRATED=ON"], 1,
     "BUILD_INTEGRATED requires BUILD_DISPLAY=ON"),
]
# 需要 SDL dev 包的成功组合 (SDL_PREFIX 未设时 SKIP 并注明原因)
SDL_COMBO = ("ai_on", ["BUILD_TESTS=ON", "BUILD_AI=ON",
                       "BUILD_AUDIO=ON", "BUILD_PROJECTION=ON"],
             0, None)
SDL_PREFIX = os.environ.get("SDL_PREFIX", "")


def log(lines, msg):
    print(msg)
    lines.append(msg)


def detect_generators():
    """返回候选 generator 列表 (新→旧)。
    Windows: 已安装的 VS generator 优先 (cmake --help 顺序即新→旧),
    预检择优; Ninja 仅当 cl/g++ 在 PATH。其他平台: Ninja 优先。
    环境变量 CMAKE_GENERATOR 显式指定时只用它。"""
    env_gen = os.environ.get("CMAKE_GENERATOR", "").strip()
    if env_gen:
        return [env_gen]
    if sys.platform.startswith("win"):
        try:
            help_out = subprocess.run([CMAKE, "--help"], capture_output=True,
                                      text=True, timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            return []
        cands = re.findall(r"^\*?\s*(Visual Studio \d+ \d{4})\b",
                           help_out, re.M)
        if shutil.which("cl") or shutil.which("g++"):
            cands.append("Ninja")
        return cands
    if shutil.which("ninja"):
        return ["Ninja", "Unix Makefiles"]
    return ["Unix Makefiles"]


def preflight(gen):
    """返回 (ok, detail): 编译器探针 = 用选定 generator 空配置一次真实项目。"""
    with tempfile.TemporaryDirectory(prefix="holopet_preflight_") as td:
        build = Path(td) / "b"
        cmd = [CMAKE, "-S", str(ROOT), "-B", str(build), "-G", gen]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            return False, "preflight configure timeout"
        out = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0:
            return False, "preflight configure failed (toolchain?)\n" + \
                "\n".join(out.splitlines()[-10:])
        m = re.search(r"The CXX compiler identification is (\S+)", out)
        comp_id = m.group(1) if m else "unknown"
        m2 = re.search(r"CMAKE_SIZEOF_VOID_P=8", out) or \
             re.search(r"x64|amd64|AMD64", out, re.I)
        arch = "x64" if m2 else "x86-or-unknown"
        return True, f"compiler={comp_id} arch={arch} generator={gen}"


def run_configure(gen, defines, need_sdl):
    with tempfile.TemporaryDirectory(prefix="holopet_matrix_") as td:
        build = Path(td) / "b"
        cmd = [CMAKE, "-S", str(ROOT), "-B", str(build), "-G", gen]
        for d in defines:
            cmd += [f"-D{d}"]
        if need_sdl and SDL_PREFIX:
            cmd += [f"-DCMAKE_PREFIX_PATH={SDL_PREFIX}"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            return r.returncode, (r.stdout or "") + (r.stderr or "")
        except subprocess.TimeoutExpired:
            return -1, "configure timeout"


def source_dir_clean():
    return not (ROOT / "CMakeCache.txt").exists() and \
           not list(ROOT.glob("CMakeFiles"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--acceptance", action="store_true",
                        help="验收模式 (默认): ai_on 不得 SKIP 后 PASS")
    parser.add_argument("--allow-skip-ai", action="store_true",
                        help="开发模式: 允许跳过 ai_on (结果不得写成 G8 PASS)")
    args = parser.parse_args()
    # R3-R1: 默认验收模式; --allow-skip-ai 仅开发
    acceptance = not args.allow_skip_ai
    if args.acceptance and args.allow_skip_ai:
        print("--acceptance 与 --allow-skip-ai 互斥")
        sys.exit(2)
    mode = "ACCEPTANCE" if acceptance else "DEV (allow-skip-ai)"

    lines = []
    failed = False

    # 0. 路径长度检查 (R3: 主动提示, 不让环境错误伪装成代码错误)
    root_len = len(str(ROOT))
    log(lines, f"ROOT={ROOT} (path length {root_len})")
    if root_len > PATH_FAIL:
        log(lines, f"[FAIL] 路径过长 ({root_len} > {PATH_FAIL}): "
                   f"请复制源码到短目录, 例如 {SHORT_PATH_HINT}")
        LOG_PATH.write_text("\n".join(lines), encoding="utf-8")
        sys.exit(1)
    if root_len > PATH_WARN and sys.platform.startswith("win"):
        log(lines, f"[WARN] 路径接近 Windows 上限 ({root_len} > {PATH_WARN}): "
                   f"如遇 MSBuild FileTracker 失败, 请复制到 {SHORT_PATH_HINT}")

    # 1. cmake 存在
    if not shutil.which(CMAKE):
        log(lines, f"[FAIL] cmake 不可用: {CMAKE}")
        LOG_PATH.write_text("\n".join(lines), encoding="utf-8")
        sys.exit(1)
    cmake_ver = subprocess.run([CMAKE, "--version"], capture_output=True,
                               text=True).stdout.splitlines()[0]
    log(lines, cmake_ver)

    # 2+3. generator 选择 + 工具链预检 (候选新→旧, 预检择优;
    #      全部失败 → 整轮 FAIL)
    candidates = detect_generators()
    log(lines, f"GENERATOR_CANDIDATES={candidates or '(none)'}")
    if not candidates:
        log(lines, "[FAIL] 无可用 generator (Windows 需 VS generator 或 "
                   "PATH 中有 cl/g++ 的 Ninja)")
        LOG_PATH.write_text("\n".join(lines), encoding="utf-8")
        sys.exit(1)
    gen, detail = None, ""
    for cand in candidates:
        ok, d = preflight(cand)
        log(lines, f"PREFLIGHT {cand}: {'PASS' if ok else 'FAIL'} — {d}")
        if ok:
            gen, detail = cand, d
            break
    if not gen:
        log(lines, "[FAIL] 所有候选 generator 预检失败 — 整轮矩阵不作判定; "
                   "请先修复编译环境 (Windows: VS Developer Prompt 或 vcvarsall)")
        LOG_PATH.write_text("\n".join(lines), encoding="utf-8")
        sys.exit(1)
    log(lines, f"GENERATOR={gen} (预检择优) {detail}")
    log(lines, f"SDL_PREFIX={SDL_PREFIX or '(unset)'}")

    # 4. 矩阵
    log(lines, f"MODE={mode}")
    combos = list(MATRIX)
    if SDL_PREFIX:
        combos.insert(2, SDL_COMBO)
    else:
        # R3-R1 (缺陷 C): 验收模式下 ai_on 不可执行 → 整轮 FAIL/INCOMPLETE,
        # 不得打印 PASS (开发模式 --allow-skip-ai 才允许 SKIP)
        if acceptance:
            failed = True
            log(lines, "[FAIL] ai_on: SDL_PREFIX 未设置 — 验收模式 (acceptance) "
                       "要求 6/6, ai_on 不得跳过; 整轮 INCOMPLETE/FAIL。"
                       "请安装 SDL dev 包并用 SDL_PREFIX 指定, 或显式使用 "
                       "--allow-skip-ai (开发模式, 结果不得写成 G8 PASS)")
        else:
            log(lines, "[SKIP] ai_on: SDL_PREFIX 未设置 (开发模式 allow-skip-ai; "
                       "该结果不得写成 G8 PASS)")

    for name, defines, expect, expect_text in combos:
        rc, output = run_configure(gen, defines, name == "ai_on")
        # 判定: 退出码 + (预期失败时) 诊断文字匹配
        if expect == 0:
            ok = rc == 0
            if not ok and "CXX compiler" in output:
                failed = True
                log(lines, f"[FAIL] {name}: rc={rc} — "
                           "工具链失败 (预检后仍缺编译器) → 整轮 FAIL")
                log(lines, "\n".join(output.splitlines()[-8:]))
                continue
            status = "PASS" if ok else "FAIL"
            if not ok:
                failed = True
            log(lines, f"[{status}] {name}: rc={rc} expect=0")
        else:
            matched = expect_text in output
            ok = (rc != 0) and matched
            status = "PASS" if ok else "FAIL"
            if not ok:
                failed = True
            log(lines, f"[{status}] {name}: rc={rc} expect!=0 "
                       f"诊断匹配={'yes' if matched else 'NO'}")
            log(lines, f"    期望文字: {expect_text}")
            if not matched:
                log(lines, "    --- 实际输出 (tail) ---")
                log(lines, "\n".join(output.splitlines()[-8:]))
        log(lines, f"--- {name} configure output (tail) ---")
        log(lines, "\n".join(output.splitlines()[-8:]))

    # 5. 源码目录清洁 (不得残留 CMakeCache/CMakeFiles)
    if source_dir_clean():
        log(lines, "[PASS] 源码目录清洁 (无 CMakeCache/CMakeFiles 残留)")
    else:
        failed = True
        log(lines, "[FAIL] 源码目录残留 CMakeCache/CMakeFiles — "
                   "矩阵必须使用独立临时 build 目录")

    n_pass = 6 - (0 if SDL_PREFIX else 1)
    verdict = "FAIL" if failed else ("PASS" if n_pass == 6 else "INCOMPLETE")
    count = "6/6" if SDL_PREFIX else str(n_pass) + "/6 (ai_on 未执行)"
    # 判定行必须进日志 (证据完整性; 不依赖调用方是否重定向 stdout)
    log(lines, f"matrix log: {LOG_PATH}")
    log(lines, f"=== 矩阵结果: {verdict} ({count}, mode={mode}) ===")
    # R4-R1: 写日志加锁重试 (瞬时文件锁不整轮失败)
    for _attempt in range(3):
        try:
            LOG_PATH.write_text("\n".join(lines), encoding="utf-8")
            break
        except PermissionError:
            import time as _t
            _t.sleep(2)
    print(f"=== 矩阵结果: {verdict} ({count}, mode={mode}) ===")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
