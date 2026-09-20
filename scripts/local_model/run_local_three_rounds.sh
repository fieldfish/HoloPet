#!/usr/bin/env bash
# run_local_three_rounds.sh — R8_R4_R3_R4_R5_R3 工作包 A 本地三轮真实文字对话驱动
# (继承 R8_R4_R5 §12-E, 已在 Pi 实机验证)。
#
# 用法: bash run_local_three_rounds.sh <HoloPet根目录> <数据目录>
# 前提: llama-server 已运行 (127.0.0.1:8080); agentd 用该根目录启动。
# 三轮: 1 自然对话 / 2 自然对话 / 3 本地功能 (记一条便签) — 全部经
# 真实 C++ 文字轮 (text-once) 走 ROUTE=local_llm, 无 Fake。
# 零云端调用保证: HOLOPET_OFFLINE=1 → 路由强制 local_llm, 不发起任何
# 外网请求; 本地模型失败不得静默回云冒充本地成功。
set -Eeuo pipefail
ROOT="${1:?usage: run_local_three_rounds.sh <root> <data>}"
D="${2:?usage: run_local_three_rounds.sh <root> <data>}"
SOCK="$D/agent.sock"
mkdir -p "$D/data"

curl -fsS http://127.0.0.1:8080/health >/dev/null || { echo FAIL_LLAMA; exit 1; }

export HOLOPET_OFFLINE=1
export HOLOPET_LOCAL_LLM_KEY=local
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$ROOT/agent" \
    nohup python3 -m holopet_agentd --config "$D/agentd.json" \
    --uds "$SOCK" --log-level INFO > "$D/agentd.log" 2>&1 &
APID=$!
i=0
while [ $i -lt 100 ]; do [ -e "$SOCK" ] && break; kill -0 $APID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
[ -e "$SOCK" ] || { echo FAIL_AGENTD; tail -5 "$D/agentd.log"; exit 1; }
echo AGENTD_READY

run_round() {
    local name="$1" q="$2" d
    d="$D/$name"
    mkdir -p "$d"
    printf '%s' "$q" > "$d/q.txt"
    DISPLAY=:0 SDL_VIDEODRIVER=x11 HOLOPET_DATA_DIR="$D/data/features.json" \
        nohup "$ROOT/build-pi-ai/holopet_ai" --worker-uds "$SOCK" \
        --text-once-file "$d/q.txt" --text-event-log "$d/audit.jsonl" \
        --text-page-ms 100 --text-final-hold-ms 100 > "$d/cpp.log" 2>&1 &
    local cpid=$! w=0 rc=0
    while kill -0 $cpid 2>/dev/null && [ $w -lt 120 ]; do sleep 0.5; w=$((w+1)); done
    if kill -0 $cpid 2>/dev/null; then kill $cpid 2>/dev/null; echo "$name=TIMEOUT"; return 1; fi
    wait $cpid || rc=$?
    echo "$name rc=$rc $(grep -o '"terminal_result":"[a-z]*"' "$d/audit.jsonl" | tail -1)"
}

run_round r1 "离线介绍一下你自己吧"
run_round r2 "给我一个健康饮食的小建议"
run_round r3 "请调用工具 add_note 帮我记一条便签：下午三点取快递"
echo "=== 路由 ==="
grep -o '"ROUTE": "[a-z_]*"' "$D/agentd.log" | sort | uniq -c
echo "=== 便签落盘 ==="
cat "$D/data/features.json" 2>/dev/null | grep -o '"text":"[^"]*"' | head -3
echo THREE_ROUNDS_DONE
