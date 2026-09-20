#!/usr/bin/env bash
# ============================================================
# install_service.sh — 按实际环境生成并安装 systemd 服务  V6-R2
# (不硬编码 User=pi / /home/pi/HoloPet; 用模板变量生成)
#
# 用法:
#   sudo ./scripts/install_service.sh [USER] [PROJECT_DIR] [PYTHON]
#   默认: USER=$(logname 或当前用户)  PROJECT_DIR=$PWD  PYTHON=/usr/bin/python3
#
# 同时 install/enable/start worker 与 main; 失败退出非零;
# 提供 journalctl 检查与卸载/回滚命令说明。
# ============================================================
set -eu

APP_USER="${1:-${SUDO_USER:-$(id -un)}}"
PROJECT_DIR="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
PYTHON_BIN="${3:-/usr/bin/python3}"
UNIT_DIR="/etc/systemd/system"
CONF_DIR="/etc/holopet"
WORKER_UNIT="holopet-ai-worker.service"
MAIN_UNIT="holopet.service"
LOCAL_LLM_UNIT="holopet-local-llm.service"
# R8_R4_R3_R4_R5_R3: 本地模型路径按 APP_USER 的 home 解析 —
# sudo 下 $HOME=/root, 直接用 $HOME 会生成 /root/... 的坏 unit
APP_HOME="$(getent passwd "$APP_USER" | cut -d: -f6)"
if [ -z "$APP_HOME" ]; then
    echo "[install_service] FAIL: 无法解析用户 home: $APP_USER" >&2
    exit 1
fi
MODEL_DIR="${MODEL_DIR:-$APP_HOME/holopet_local_model}"
LLAMA_BIN="${LLAMA_BIN:-$APP_HOME/llama.cpp/build/bin/llama-server}"
LLAMA_THREADS="${LLAMA_THREADS:-4}"

log() { echo "[install_service] $*"; }
die() { echo "[install_service] FAIL: $*" >&2; exit 1; }

# ---- 安装前检查 ----
[ "$(id -u)" -eq 0 ] || die "需要 root (sudo ./scripts/install_service.sh)"
[ -d "$PROJECT_DIR" ] || die "项目目录不存在: $PROJECT_DIR"
[ -f "$PROJECT_DIR/agent/holopet_agentd/main.py" ] || die "worker 源码缺失: $PROJECT_DIR/agent"
[ -x "$PYTHON_BIN" ] || die "Python 不可执行: $PYTHON_BIN"
id "$APP_USER" >/dev/null 2>&1 || die "用户不存在: $APP_USER"

# ---- 配置文件 ----
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/holopet.env" ]; then
    cp "$PROJECT_DIR/config/holopet.example.env" "$CONF_DIR/holopet.env" || \
        die "复制示例 env 失败"
    chmod 600 "$CONF_DIR/holopet.env"
    log "已生成 $CONF_DIR/holopet.env (chmod 600, 请填入真实值)"
fi
if [ ! -f "$CONF_DIR/agent.json" ]; then
    cp "$PROJECT_DIR/config/agent.example.json" "$CONF_DIR/agent.json" || \
        die "复制 agent.json 失败"
    log "已生成 $CONF_DIR/agent.json"
fi

# ---- 主程序二进制探测 (R8_R4_R3_R4_R5_R3: Pi 上为 build-pi-ai, 回退 build-ai) ----
MAIN_BIN=""
for _cand in "$PROJECT_DIR/build-pi-ai/holopet_ai" \
             "$PROJECT_DIR/build-ai/holopet_ai"; do
    if [ -x "$_cand" ]; then MAIN_BIN="$_cand"; break; fi
done
if [ -z "$MAIN_BIN" ]; then
    MAIN_BIN="$PROJECT_DIR/build-pi-ai/holopet_ai"
    echo "[install_service] WARN: 主程序缺失 $MAIN_BIN (需先构建 AI 变体)"
fi

# ---- 模板生成 (sed 替换占位符) ----
sed -e "s|@APP_USER@|$APP_USER|g" \
    -e "s|@PROJECT_DIR@|$PROJECT_DIR|g" \
    -e "s|@PYTHON_BIN@|$PYTHON_BIN|g" \
    "$PROJECT_DIR/systemd/holopet-ai-worker.service.in" > "$UNIT_DIR/$WORKER_UNIT" || \
    die "生成 worker unit 失败"

sed -e "s|@APP_USER@|$APP_USER|g" \
    -e "s|@PROJECT_DIR@|$PROJECT_DIR|g" \
    -e "s|@MAIN_BIN@|$MAIN_BIN|g" \
    "$PROJECT_DIR/systemd/holopet.service.in" > "$UNIT_DIR/$MAIN_UNIT" || \
    die "生成 main unit 失败"

sed -e "s|@APP_USER@|$APP_USER|g" \
    -e "s|@PROJECT_DIR@|$PROJECT_DIR|g" \
    -e "s|@MODEL_DIR@|$MODEL_DIR|g" \
    -e "s|@LLAMA_BIN@|$LLAMA_BIN|g" \
    -e "s|@LLAMA_THREADS@|$LLAMA_THREADS|g" \
    "$PROJECT_DIR/systemd/holopet-local-llm.service.in" > "$UNIT_DIR/$LOCAL_LLM_UNIT" || \
    die "生成 local-llm unit 失败"

# ---- 安装 / enable / start ----
systemctl daemon-reload || die "daemon-reload 失败"
# R8_R4_R3_R4_R5_R3: local-llm 可独立失败 (云端模式不受影响)
if [ -x "$LLAMA_BIN" ] && [ -f "$MODEL_DIR/qwen2.5-1.5b-instruct-q4_k_m.gguf" ]; then
    systemctl enable --now "$LOCAL_LLM_UNIT" || \
        echo "[install_service] WARN: local-llm 启动失败 (云端模式仍可用)"
else
    echo "[install_service] WARN: 本地模型/llama.cpp 未就绪, 跳过 local-llm (可后补安装)"
fi
systemctl enable --now "$WORKER_UNIT" || die "worker enable/start 失败"
sleep 1
systemctl is-active --quiet "$WORKER_UNIT" || die "worker 未运行 (journalctl -u $WORKER_UNIT)"
systemctl enable --now "$MAIN_UNIT" || {
    echo "[install_service] WARN: main 启动失败 (检查显示会话与构建产物)"
}
systemctl daemon-reload

# ---- 完成提示 ----
cat <<EOF

[install_service] 安装完成
  用户:       $APP_USER
  项目目录:   $PROJECT_DIR
  配置:       $CONF_DIR/holopet.env (600) / agent.json
  unit 文件:  $UNIT_DIR/{$WORKER_UNIT,$MAIN_UNIT,$LOCAL_LLM_UNIT}

  检查日志:
    journalctl -u holopet-ai-worker -f
    journalctl -u holopet -f
    journalctl -u holopet-local-llm -f
  状态:
    systemctl status holopet-ai-worker holopet holopet-local-llm
  卸载/回滚:
    sudo systemctl disable --now holopet holopet-ai-worker holopet-local-llm
    sudo rm -f /etc/systemd/system/holopet.service \\
               /etc/systemd/system/holopet-ai-worker.service \\
               /etc/systemd/system/holopet-local-llm.service
    sudo systemctl daemon-reload
    sudo rm -rf /etc/holopet
EOF
