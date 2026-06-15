#!/usr/bin/env bash
# Build mqtt_min_broker inside Docker — zero host pollution.
# Artifacts land in ./build/ after the run.
#
# Usage:
#   ./docker-build.sh              # normal build; reuses env image if it exists
#   REBUILD_ENV=1 ./docker-build.sh  # force-rebuild the Zephyr env image (~30 min)
#   BOARD=esp32s3 ./docker-build.sh  # target a different board
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_IMAGE="mqtt_min_broker_env:latest"
BOARD="${BOARD:-esp32}"

_info() { echo "[docker-build] $*"; }

if ! command -v docker &>/dev/null; then
    echo "error: docker not found" >&2; exit 1
fi

# ── build env image only when needed ─────────────────────────────────────────
if [[ "${REBUILD_ENV:-0}" == "1" ]] || ! docker image inspect "$ENV_IMAGE" &>/dev/null; then
    _info "Building Zephyr env image (first run ~30 min)..."
    docker build \
        --platform linux/amd64 \
        -t "$ENV_IMAGE" \
        -f "$SCRIPT_DIR/Dockerfile.env" \
        "$SCRIPT_DIR"
else
    _info "Env image '$ENV_IMAGE' already exists — skipping rebuild. (set REBUILD_ENV=1 to force)"
fi

# ── run west build with project source mounted ────────────────────────────────
_info "Running west build -b $BOARD ..."
docker run --rm \
    --platform linux/amd64 \
    -v "$SCRIPT_DIR:/workspace" \
    "$ENV_IMAGE" \
    west build -b "$BOARD" /workspace

# ── copy flashable artifacts to build_out/ ─────────────────────────────────────────
mkdir -p "$SCRIPT_DIR/build_out"
cp "$SCRIPT_DIR/build/zephyr/zephyr.bin"       "$SCRIPT_DIR/build_out/"
cp "$SCRIPT_DIR/build/zephyr/zephyr.elf"       "$SCRIPT_DIR/build_out/"
cp "$SCRIPT_DIR/build/zephyr/zephyr_final.map" "$SCRIPT_DIR/build_out/"
_info "Done. Firmware in $SCRIPT_DIR/build_out/"
