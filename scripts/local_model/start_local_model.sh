#!/usr/bin/env bash
# start_local_model.sh — R8_R4 B7 本地 llama.cpp 服务启动 (Pi 执行, 离线可用)。
# 启动前校验模型 SHA256 绑定; 服务暴露 http://127.0.0.1:8080/v1 (OpenAI 兼容)。
# 用法: start_local_model.sh [MODEL_DIR] [LLAMA_BIN] [PORT] [N_THREADS]
set -Eeuo pipefail

MODEL_DIR="${1:-$HOME/holopet_local_model}"
LLAMA_BIN="${2:-$HOME/llama.cpp/build/bin/llama-server}"
PORT="${3:-8080}"
N_THREADS="${4:-4}"
FILE="$MODEL_DIR/qwen2.5-1.5b-instruct-q4_k_m.gguf"
META="$MODEL_DIR/model.meta.json"

[ -f "$FILE" ] || { echo "FAIL: 模型文件缺失 $FILE (先跑 download_local_model.sh)" >&2; exit 1; }
[ -x "$LLAMA_BIN" ] || { echo "FAIL: llama-server 缺失 $LLAMA_BIN" >&2; exit 1; }

if [ -f "$META" ]; then
    # R8_R4_R3_R4_R5_R3: meta 为真实 JSON (下载端已修); 读取端先验 JSON 合法。
    # python3 在部分环境只是占位 stub (退出非 0 无输出), 先验可用再选解释器。
    PY="python3"
    python3 -c "import json" >/dev/null 2>&1 || PY="python"
    "$PY" -c "import json,sys; json.load(open(sys.argv[1]))" "$META" 2>/dev/null         || { echo "FAIL: model.meta.json 非法 JSON (拒绝启动)" >&2; exit 1; }
    WANT="$(grep -o '"sha256": *"[a-f0-9]*"' "$META" | head -1 | cut -d'"' -f4)"
    GOT="$(sha256sum "$FILE" | cut -d' ' -f1)"
    [ -n "$WANT" ] && [ "$WANT" = "$GOT" ]         || { echo "FAIL: SHA256 绑定不一致 want=$WANT got=$GOT (拒绝启动)" >&2; exit 1; }
    MB="$(grep -o '"bytes": *[0-9]*' "$META" | head -1 | grep -o '[0-9]*')"
    echo "IDENTITY_BOUND=yes sha256=$GOT bytes=$MB"
fi

# R8_R4_R3_R4_R5_R3 (C): --foreground (第5参) = systemd 托管模式 —
# 主进程即 llama-server (Type=simple 跟踪正确), 崩溃由 Restart=on-failure
# 接管; 校验仍先执行。不带该参数保持交互式后台+健康检查行为。
FOREGROUND=0
if [ "${5:-}" = "--foreground" ]; then FOREGROUND=1; fi

if [ "$FOREGROUND" = "1" ]; then
    echo "LOCAL_MODEL_FOREGROUND port=$PORT pid=$$"
    exec "$LLAMA_BIN" -m "$FILE" --host 127.0.0.1 --port "$PORT" \
        --ctx-size 4096 --threads "$N_THREADS" --n-gpu-layers 0
fi

# 非阻塞后台启动; 有界等待健康检查 (最多 60s)
"$LLAMA_BIN" -m "$FILE" --host 127.0.0.1 --port "$PORT" \
    --ctx-size 4096 --threads "$N_THREADS" --n-gpu-layers 0 \
    > "$MODEL_DIR/llama-server.log" 2>&1 &
SRV_PID=$!
echo "LLAMA_SERVER_PID=$SRV_PID"

for i in $(seq 1 120); do
    if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        echo "LOCAL_MODEL_READY port=$PORT"
        exit 0
    fi
    kill -0 "$SRV_PID" 2>/dev/null || { echo "FAIL: llama-server 退出 (见日志)"; exit 1; }
    sleep 0.5
done
echo "FAIL: 健康检查超时 (60s)"
exit 1
