#!/usr/bin/env bash
# download_local_model.sh — R8_R4 B7 本地模型下载与 SHA256 身份绑定 (Pi 执行)。
# 模型权重在交付包外 (~/holopet_local_model); 不把权重塞入源码包。
# 用法: download_local_model.sh [MODEL_DIR] [HF_REPO] [FILE] [EXPECTED_SHA256]
set -Eeuo pipefail

MODEL_DIR="${1:-$HOME/holopet_local_model}"
HF_REPO="${2:-Qwen/Qwen2.5-1.5B-Instruct-GGUF}"
FILE="${3:-qwen2.5-1.5b-instruct-q4_k_m.gguf}"
EXPECTED_SHA256="${4:-}"

mkdir -p "$MODEL_DIR"
cd "$MODEL_DIR"

if [ -f "$FILE" ]; then
    echo "已存在: $FILE (跳过下载)"
else
    URL="https://huggingface.co/$HF_REPO/resolve/main/$FILE"
    echo "下载: $URL"
    curl -fL --retry 3 -o "$FILE.tmp" "$URL"
    mv "$FILE.tmp" "$FILE"
fi

if [ -n "$EXPECTED_SHA256" ]; then
    GOT="$(sha256sum "$FILE" | cut -d' ' -f1)"
    if [ "$EXPECTED_SHA256" != "$GOT" ]; then
        echo "FAIL: SHA256 不匹配 expected=$EXPECTED_SHA256 got=$GOT" >&2
        exit 1
    fi
fi

GOT="$(sha256sum "$FILE" | cut -d' ' -f1)"
BYTES="$(stat -c %s "$FILE" 2>/dev/null || stat -f %z "$FILE")"
# R8_R4_R3_R4_R5_R3: model.meta.json 必须为真实 JSON (启动端按 JSON 读取);
# 临时文件 + 原子更名, 避免半写状态。
{
    printf '{
'
    printf '  "model_file": "%s",
' "$FILE"
    printf '  "hf_repo": "%s",
' "$HF_REPO"
    printf '  "sha256": "%s",
' "$GOT"
    printf '  "bytes": %s,
' "$BYTES"
    printf '  "downloaded_at": "%s"
' "$(date -Is)"
    printf '}
'
} > model.meta.json.tmp
mv model.meta.json.tmp model.meta.json

echo "MODEL_READY sha256=$GOT bytes=$BYTES"
