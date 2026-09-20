#!/usr/bin/env bash
# ============================================================
# audio_bench.sh — USB Audio 检测与录放音基准  V6
# 用法 (Raspberry Pi):
#   chmod +x scripts/audio_bench.sh
#   ./scripts/audio_bench.sh            # 检测 + 默认参数基准
#   ./scripts/audio_bench.sh -d 2 -r 3  # 指定卡号 2, 录音 3 秒
# 输出: 设备列表 / 录音文件 / 播放确认 / 电平统计 (sox 可用时)
# ============================================================
set -u

CARD=""
RECORD_SEC=3
SAMPLE_RATE=16000
# R2: wav 只写明确 work-dir (--work-dir 或 HOLOPET_AUDIO_TMP);
# 调用方登记所有权并在退出/中断时清理, 保证交付前 wav 为零。
WORKDIR="${HOLOPET_AUDIO_TMP:-${TMPDIR:-/tmp}/holopet_audio_bench}"

usage() {
    echo "usage: $0 [-d CARD] [-r SECONDS] [-s SAMPLE_RATE] [--work-dir DIR]"
    echo "  -d CARD         ALSA 卡号 (如 2; 不填则自动探测 USB Audio)"
    echo "  -r SECONDS      录音时长 (默认 3)"
    echo "  -s SAMPLE_RATE  采样率 (默认 16000)"
    echo "  --work-dir DIR  wav 输出目录 (默认 \$TMPDIR/holopet_audio_bench)"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -d) CARD="$2"; shift 2 ;;
        -r) RECORD_SEC="$2"; shift 2 ;;
        -s) SAMPLE_RATE="$2"; shift 2 ;;
        --work-dir) WORKDIR="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

mkdir -p "$WORKDIR"
REC_FILE="$WORKDIR/bench_$(date +%s).wav"

echo "=== 1. 设备检测 (aplay -l) ==="
aplay -l 2>&1 || { echo "FAIL: aplay 不可用 (安装 alsa-utils)"; exit 1; }
echo
echo "=== 2. 录音设备检测 (arecord -l) ==="
arecord -l 2>&1 || { echo "FAIL: arecord 不可用"; exit 1; }
echo

# 自动探测 USB Audio 卡号 (优先)
if [ -z "$CARD" ]; then
    CARD=$(arecord -l 2>/dev/null | grep -i "USB Audio" | head -1 | sed -E 's/^card ([0-9]+).*/\1/')
    if [ -z "$CARD" ]; then
        CARD=$(arecord -l 2>/dev/null | head -2 | tail -1 | sed -E 's/^card ([0-9]+).*/\1/')
        echo "WARN: 未找到 USB Audio, 使用卡 $CARD (请核对)"
    else
        echo "检测到 USB Audio: card $CARD"
    fi
fi

DEV="plughw:$CARD,0"
echo
echo "=== 3. 录音基准 ($RECORD_SEC s @ $SAMPLE_RATE Hz, $DEV) ==="
arecord -D "$DEV" -f S16_LE -r "$SAMPLE_RATE" -c 1 -d "$RECORD_SEC" "$REC_FILE" \
    && echo "PASS: 录音完成 → $REC_FILE" \
    || { echo "FAIL: 录音失败 (检查 -D 参数/接线)"; exit 1; }
echo
echo "=== 4. 播放基准 (扬声器回放) ==="
aplay -D "$DEV" "$REC_FILE" \
    && echo "PASS: 播放完成" \
    || { echo "FAIL: 播放失败 (检查播放设备/音量)"; exit 1; }
echo
echo "=== 5. 电平统计 (sox 可用时) ==="
if command -v sox >/dev/null 2>&1; then
    sox "$REC_FILE" -n stat 2>&1 | grep -E "Maximum amplitude|RMS"
else
    echo "SKIP: 未安装 sox (sudo apt install sox)"
fi
echo
echo "=== 结果 ==="
echo "card=$CARD device=$DEV record=$REC_FILE"
echo "建议把 -D $DEV 写入 config/holopet.env 的 HOLOPET_RECORD_DEVICE/HOLOPET_PLAY_DEVICE"
echo "Physical audio = WAITING (实机听感/降噪由实测确认)"
