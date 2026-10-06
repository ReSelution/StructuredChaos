#!/usr/bin/env bash
set -euo pipefail

XWIN_BIN="$(which xwin)"
TARGET_DIR="/opt/win-sdk/10.0.22621"
CACHE_DIR="/opt/win-sdk/.xwin-cache"

if [ -z "$XWIN_BIN" ]; then
  echo "Error: xwin was not found in the current PATH."
  exit 1
fi

echo "Starting xwin via sudo from: $XWIN_BIN"

sudo "$XWIN_BIN" \
  --accept-license \
  --variant desktop \
  --arch x86_64 \
  --include-atl \
  --cache-dir "$CACHE_DIR" \
  splat \
  --preserve-ms-arch-notation \
  --include-debug-libs \
  --output "$TARGET_DIR"

echo "SDK successfully downloaded and unpacked."
