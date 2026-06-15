#!/usr/bin/env bash
# Build mqtt_min_broker inside Docker — zero host pollution.
# Artifacts land in ./build_out/ with version + date stamp.
#
# Usage:
#   ./docker-build.sh                    # normal build; reuses env image if it exists
#   REBUILD_ENV=1 ./docker-build.sh      # force-rebuild the Zephyr env image (~30 min)
#   BOARD=esp32s3 ./docker-build.sh      # target a different board
#   VERSION=1.2.3 ./docker-build.sh      # override version (default: reads ./VERSION)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_IMAGE="mqtt_min_broker_env:latest"
BOARD="${BOARD:-esp32}"
_version_from_file() {
    awk '
        /^VERSION_MAJOR/ { maj=$3 }
        /^VERSION_MINOR/ { min=$3 }
        /^PATCHLEVEL/    { patch=$3 }
        END {
            if (maj != "") print maj "." min "." patch;
            else print "dev";
        }
    ' "$SCRIPT_DIR/VERSION" 2>/dev/null
}
VERSION="${VERSION:-$(_version_from_file)}"
DATE="$(date +%Y%m%d)"
STAMP="v${VERSION}_${DATE}"

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
_info "Running west build -b $BOARD (version $STAMP)..."
docker run --rm \
    --platform linux/amd64 \
    -v "$SCRIPT_DIR:/workspace" \
    "$ENV_IMAGE" \
    west build -b "$BOARD" /workspace

# ── copy flashable artifacts to build_out/ with stamp ────────────────────────
mkdir -p "$SCRIPT_DIR/build_out"

BIN="$SCRIPT_DIR/build_out/zephyr_${STAMP}.bin"
ELF="$SCRIPT_DIR/build_out/zephyr_${STAMP}.elf"
MAP="$SCRIPT_DIR/build_out/zephyr_${STAMP}.map"

cp "$SCRIPT_DIR/build/zephyr/zephyr.bin"       "$BIN"
cp "$SCRIPT_DIR/build/zephyr/zephyr.elf"       "$ELF"
cp "$SCRIPT_DIR/build/zephyr/zephyr_final.map" "$MAP"

# update "latest" symlinks
ln -sf "zephyr_${STAMP}.bin" "$SCRIPT_DIR/build_out/zephyr.bin"
ln -sf "zephyr_${STAMP}.elf" "$SCRIPT_DIR/build_out/zephyr.elf"
ln -sf "zephyr_${STAMP}.map" "$SCRIPT_DIR/build_out/zephyr.map"

_info "Done."
_info "  firmware → build_out/zephyr_${STAMP}.bin"
_info "  symlink  → build_out/zephyr.bin"
