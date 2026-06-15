#!/usr/bin/env bash
# Build mqtt_min_broker inside Docker — zero host pollution.
# Artifacts land in ./build/ after the run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="mqtt_min_broker_build:latest"
BOARD="${BOARD:-esp32}"

_info() { echo "[docker-build] $*"; }

if ! command -v docker &>/dev/null; then
    echo "error: docker not found" >&2; exit 1
fi

# ── build image (uses layer cache; only re-runs changed layers) ───────────────
_info "Building Docker image (first run takes ~30 min)..."
docker build \
    --platform linux/amd64 \
    -t "$IMAGE" \
    -f "$SCRIPT_DIR/Dockerfile.build" \
    "$SCRIPT_DIR"

# ── run west build with project source mounted ────────────────────────────────
_info "Running west build -b $BOARD ..."
docker run --rm \
    --platform linux/amd64 \
    -v "$SCRIPT_DIR:/workspace" \
    "$IMAGE" \
    west build -b "$BOARD" /workspace

_info "Done. Artifacts in $SCRIPT_DIR/build/"
