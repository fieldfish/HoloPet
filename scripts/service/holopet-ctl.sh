#!/usr/bin/env bash
# holopet-ctl.sh — 常驻宠物服务控制 (R8_R3_R3_R2 §6.1; R8_R4_R3_R4_R5_R3 三服务)。
#
# 用法: holopet-ctl.sh install|start|stop|restart|status|uninstall
#   [APP_USER] [PROJECT_DIR] [PYTHON_BIN]
# install 需要 sudo (用户本地输入密码, 脚本绝不索取/保存); 其余操作
# 亦需 sudo (系统级 unit); status 无需特权。
# 服务等待图形会话/音频/网络可用性由 unit 的 Restart 有界重试承担
# (RestartSec 有界, 不无限快速重启)。
# R8_R4_R3_R4_R5_R3: 同时管理 worker/main/local-llm 三服务;
# local-llm 未安装时其余操作不受影响 (仅提示)。
set -Eeuo pipefail

CMD="${1:-status}"
APP_USER="${2:-${SUDO_USER:-$(id -un)}}"
PROJECT_DIR="${3:-$(cd "$(dirname "$0")/../.." && pwd)}"
PYTHON_BIN="${4:-/usr/bin/python3}"
UNIT_MAIN="holopet.service"
UNIT_WORKER="holopet-ai-worker.service"
UNIT_LOCAL_LLM="holopet-local-llm.service"

unit_installed() {
    # R8_R4_R3_R4_R5_R3 (v7): 无管道匹配 — pipefail 下 grep -q 提前退出
    # 会给 systemctl 送 SIGPIPE 造成竞态性误判
    local out
    out="$(systemctl list-unit-files "$1" 2>/dev/null || true)"
    case "$out" in
        *"$1"*) return 0 ;;
        *) return 1 ;;
    esac
}

case "$CMD" in
    install)
        sudo "$PROJECT_DIR/scripts/install_service.sh" \
            "$APP_USER" "$PROJECT_DIR" "$PYTHON_BIN"
        ;;
    start)
        sudo systemctl start "$UNIT_WORKER"
        sleep 1
        if unit_installed "$UNIT_LOCAL_LLM"; then
            sudo systemctl start "$UNIT_LOCAL_LLM" || \
                echo "[holopet-ctl] WARN: local-llm 启动失败 (云端模式不受影响)"
        fi
        sudo systemctl start "$UNIT_MAIN" || true
        ;;
    stop)
        sudo systemctl stop "$UNIT_MAIN" "$UNIT_WORKER" 2>/dev/null || true
        if unit_installed "$UNIT_LOCAL_LLM"; then
            sudo systemctl stop "$UNIT_LOCAL_LLM" 2>/dev/null || true
        fi
        ;;
    restart)
        sudo systemctl restart "$UNIT_WORKER"
        sleep 1
        if unit_installed "$UNIT_LOCAL_LLM"; then
            sudo systemctl restart "$UNIT_LOCAL_LLM" 2>/dev/null || \
                echo "[holopet-ctl] WARN: local-llm 重启失败 (云端模式不受影响)"
        fi
        sudo systemctl restart "$UNIT_MAIN" || true
        ;;
    status)
        echo "worker:     $(systemctl is-active "$UNIT_WORKER" 2>/dev/null || echo not-installed)"
        echo "main:       $(systemctl is-active "$UNIT_MAIN" 2>/dev/null || echo not-installed)"
        if unit_installed "$UNIT_LOCAL_LLM"; then
            echo "local-llm:  $(systemctl is-active "$UNIT_LOCAL_LLM" 2>/dev/null || echo not-installed)"
        else
            echo "local-llm:  not-installed (云端模式)"
        fi
        systemctl --no-pager -n 5 status "$UNIT_WORKER" 2>/dev/null | tail -5 || true
        ;;
    uninstall)
        sudo systemctl disable --now "$UNIT_MAIN" "$UNIT_WORKER" 2>/dev/null || true
        if unit_installed "$UNIT_LOCAL_LLM"; then
            sudo systemctl disable --now "$UNIT_LOCAL_LLM" 2>/dev/null || true
        fi
        sudo rm -f "/etc/systemd/system/$UNIT_MAIN" \
                   "/etc/systemd/system/$UNIT_WORKER" \
                   "/etc/systemd/system/$UNIT_LOCAL_LLM"
        sudo systemctl daemon-reload
        ;;
    *)
        echo "用法: $0 install|start|stop|restart|status|uninstall [USER] [PROJECT_DIR] [PYTHON]" >&2
        exit 2
        ;;
esac
