#!/usr/bin/env bash
# measure_local_model.sh — R8_R4_R3_R4_R5_R3 工作包 A 本地模型实机门:
#   冷启动耗时 / RSS / 温度 / meminfo (Pi 执行, 离线可用)。
#
# 用法: bash measure_local_model.sh [MODEL_DIR] [LLAMA_BIN] [PORT]
# 测量流程:
#   1. 停掉占用端口的旧 llama-server (如有; 需能 kill — 建议先
#      sudo systemctl stop holopet-local-llm 由调用方处理);
#   2. 冷启动 start_local_model.sh (后台+健康检查路径), 记录从进程
#      启动到 /health 就绪的秒数 (模型加载计入);
#   3. 采样 llama-server RSS / 系统温度 / meminfo;
#   4. 结果写 MODEL_DIR/measure.json (JSON 单文件, 无正文/密钥)。
# 输出字段: COLD_START_SEC / RSS_KB / TEMP_C / MEM_TOTAL_KB /
#   MEM_AVAILABLE_KB / MODEL_BYTES / SHA256_BOUND。
set -Eeuo pipefail

MODEL_DIR="${1:-$HOME/holopet_local_model}"
LLAMA_BIN="${2:-$HOME/llama.cpp/build/bin/llama-server}"
PORT="${3:-8080}"
FILE="$MODEL_DIR/qwen2.5-1.5b-instruct-q4_k_m.gguf"
META="$MODEL_DIR/model.meta.json"
OUT="$MODEL_DIR/measure.json"

[ -f "$FILE" ] || { echo "FAIL: 模型文件缺失 $FILE" >&2; exit 1; }
[ -x "$LLAMA_BIN" ] || { echo "FAIL: llama-server 缺失 $LLAMA_BIN" >&2; exit 1; }
curl --version >/dev/null 2>&1 || { echo "FAIL: curl 缺失" >&2; exit 1; }

# ---- 预检: 端口是否已被占用 (拒绝覆盖测量语义) ----
if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
    echo "NOTE: 端口 $PORT 已有健康服务 — 记录 WARM 状态并跳过冷启动"
    WARM=1
else
    WARM=0
fi

# ---- 冷启动 ----
COLD_START_SEC=""
LLAMA_PID=""
if [ "$WARM" = "1" ]; then
    COLD_START_SEC="NOT_MEASURED(warm)"
else
    T0="$(date +%s.%N)"
    bash "$(dirname "$0")/start_local_model.sh" \
        "$MODEL_DIR" "$LLAMA_BIN" "$PORT" > "$MODEL_DIR/measure_start.log" 2>&1 &
    STARTER_PID=$!
    i=0
    READY=0
    while [ $i -lt 240 ]; do
        if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
            READY=1; break
        fi
        kill -0 $STARTER_PID 2>/dev/null || break
        sleep 0.5; i=$((i+1))
    done
    T1="$(date +%s.%N)"
    if [ "$READY" = "1" ]; then
        COLD_START_SEC="$(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }')"
    else
        echo "FAIL: 冷启动健康检查超时 (120s), 见 $MODEL_DIR/measure_start.log" >&2
        exit 1
    fi
fi

# ---- 采样 (RSS/温度/meminfo) ----
LLAMA_PID="$(pgrep -f "llama-server -m $FILE" | head -1 || true)"
[ -n "$LLAMA_PID" ] || LLAMA_PID="$(pgrep -f 'llama-server' | head -1 || true)"
RSS_KB="$(ps -o rss= -p "$LLAMA_PID" 2>/dev/null | tr -d ' ' || true)"
case "$RSS_KB" in ''|*[!0-9]*) RSS_KB=-1;; esac
[ -n "$LLAMA_PID" ] || LLAMA_PID=-1
TEMP_C="$(vcgencmd measure_temp 2>/dev/null | grep -o '[0-9.]*' | head -1 || echo "UNKNOWN")"
MEM_TOTAL_KB="$(awk '/MemTotal/ {print $2}' /proc/meminfo)"
MEM_AVAILABLE_KB="$(awk '/MemAvailable/ {print $2}' /proc/meminfo)"
MODEL_BYTES="$(stat -c %s "$FILE" 2>/dev/null || echo "UNKNOWN")"
SHA_BOUND=""
if [ -f "$META" ]; then
    SHA_BOUND="$(grep -o '"sha256": *"[a-f0-9]*"' "$META" | head -1 | cut -d'"' -f4)"
fi

# ---- JSON 输出 (原子写) ----
cat > "$OUT.tmp" <<EOF
{
  "measured_at": "$(date -Is)",
  "cold_start_sec": "$COLD_START_SEC",
  "rss_kb": $RSS_KB,
  "temp_c": $TEMP_C,
  "mem_total_kb": $MEM_TOTAL_KB,
  "mem_available_kb": $MEM_AVAILABLE_KB,
  "model_bytes": $MODEL_BYTES,
  "sha256_bound": "$SHA_BOUND",
  "llama_pid": $LLAMA_PID,
  "port": $PORT
}
EOF
mv "$OUT.tmp" "$OUT"
echo "MEASURE_DONE $OUT"
cat "$OUT"
