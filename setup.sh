#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_JSON="$SCRIPT_DIR/deps.json"
BUILD_DIR="$SCRIPT_DIR/build"

_require() {
    if ! command -v "$1" &>/dev/null; then
        echo "error: '$1' not found — $2" >&2
        exit 1
    fi
}

_json() {
    jq -r "$1" "$DEPS_JSON"
}

_check_zephyr_base() {
    if [ -z "${ZEPHYR_BASE:-}" ]; then
        echo "error: ZEPHYR_BASE is not set." >&2
        echo "  Source your Zephyr env first:" >&2
        echo "    source <workspace>/zephyr/zephyr-env.sh" >&2
        exit 1
    fi
    if [ ! -d "$ZEPHYR_BASE" ]; then
        echo "error: ZEPHYR_BASE='$ZEPHYR_BASE' does not exist." >&2
        exit 1
    fi
}

_zephyr_version() {
    if [ -f "$ZEPHYR_BASE/VERSION" ]; then
        awk '
            /^VERSION_MAJOR/ { maj=$3 }
            /^VERSION_MINOR/ { min=$3 }
            /^PATCHLEVEL/    { pl=$3  }
            END { print "v" maj "." min "." pl }
        ' "$ZEPHYR_BASE/VERSION"
    else
        echo "unknown"
    fi
}

# ---------------------------------------------------------------------------
# setup: verify tools + Zephyr environment (merged prepare + init)
# ---------------------------------------------------------------------------
cmd_setup() {
    echo "[setup] Checking required tools..."
    _require jq    "install with: sudo apt install jq"
    _require cmake "install Zephyr SDK: https://docs.zephyrproject.org/latest/develop/getting_started/"
    _require west  "install with: pip install west"
    _require python3 "install with: sudo apt install python3"

    echo "[setup] Checking Zephyr environment..."
    _check_zephyr_base

    local min_ver board actual_ver
    min_ver=$(_json '.zephyr.min_version')
    board=$(_json '.build.board')
    actual_ver=$(_zephyr_version)

    echo "[setup] ZEPHYR_BASE : $ZEPHYR_BASE"
    echo "[setup] Zephyr      : ${actual_ver} (min required: ${min_ver})"
    echo "[setup] Board       : ${board}"
    echo "[setup] cmake       : $(cmake --version | head -1)"
    echo "[setup] west        : $(west --version)"
    echo "[setup] python3     : $(python3 --version)"

    echo "[setup] Checking west workspace..."
    local west_root
    west_root="$(west topdir 2>/dev/null || true)"
    if [ -z "$west_root" ]; then
        echo "[setup] warning: not inside a west workspace — run 'west init / west update' first." >&2
    else
        echo "[setup] west workspace : $west_root"
        echo "[setup] Updating west modules..."
        (cd "$west_root" && west update --narrow -o=--depth=1)
        echo "[setup] west update done."
    fi

    echo "[setup] OK — environment is ready."
}

# ---------------------------------------------------------------------------
# build: cmake configure + compile
# ---------------------------------------------------------------------------
cmd_build() {
    _require jq    "install with: sudo apt install jq"
    _require cmake "install Zephyr SDK"
    _check_zephyr_base

    local board
    board=$(_json '.build.board')

    echo "[build] Board     : ${board}"
    echo "[build] Source    : ${SCRIPT_DIR}"
    echo "[build] Build dir : ${BUILD_DIR}"

    cmake -B "$BUILD_DIR" \
          -DBOARD="$board" \
          -DZEPHYR_BASE="$ZEPHYR_BASE" \
          "$SCRIPT_DIR"

    cmake --build "$BUILD_DIR" -- -j"$(nproc)"
    echo "[build] Done."
}

# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 <command>

Commands:
  setup   Check tools + Zephyr environment, update west modules
  build   Configure and build the project
  all     setup then build

Requires ZEPHYR_BASE to be set before running.
Board and minimum Zephyr version are read from deps.json.
EOF
}

case "${1:-}" in
    setup) cmd_setup ;;
    build) cmd_build ;;
    all)   cmd_setup && cmd_build ;;
    *)     usage; exit 1 ;;
esac
