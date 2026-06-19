#!/usr/bin/env bash
# Smoke test for using this repository as a Zephyr module.
#
# The script always validates module metadata and build wiring. If west and a
# Zephyr environment are available, it also builds a temporary embedder app with
# CONFIG_MQTT_MIN_BROKER=y and CONFIG_MQTT_STANDALONE=n.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
PASS=0
FAIL=0
SKIP=0

BOARD="${ZEPHYR_BOARD:-native_sim}"
BUILD_DIR="${ZEPHYR_BUILD_DIR:-/tmp/mqtt_min_broker_zephyr_build}"
APP_DIR=""

_ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
_skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }

_cleanup() {
    if [ -n "$APP_DIR" ] && [ -d "$APP_DIR" ]; then
        rm -rf "$APP_DIR"
    fi
}
trap _cleanup EXIT

_contains() {
    local file="$1"
    local pattern="$2"
    grep -qF "$pattern" "$file"
}

echo "=== Zephyr module metadata ==="

if _contains "$ROOT/zephyr/module.yml" "cmake: zephyr" &&
   _contains "$ROOT/zephyr/module.yml" "kconfig: zephyr/Kconfig"; then
    _ok "module.yml points to zephyr CMake/Kconfig"
else
    _fail "module.yml is missing Zephyr build metadata"
fi

if _contains "$ROOT/zephyr/CMakeLists.txt" "if(CONFIG_MQTT_MIN_BROKER)" &&
   _contains "$ROOT/zephyr/CMakeLists.txt" "zephyr_library_named(mqtt_min_broker)"; then
    _ok "module CMake gates library on CONFIG_MQTT_MIN_BROKER"
else
    _fail "module CMake does not gate mqtt_min_broker library correctly"
fi

if _contains "$ROOT/zephyr/CMakeLists.txt" "zephyr_library_sources_ifdef(CONFIG_MQTT_STANDALONE" &&
   _contains "$ROOT/zephyr/CMakeLists.txt" "../src/main.c" &&
   _contains "$ROOT/zephyr/CMakeLists.txt" "../src/wifi.c"; then
    _ok "standalone main/WiFi sources are gated by CONFIG_MQTT_STANDALONE"
else
    _fail "standalone sources are not clearly gated by CONFIG_MQTT_STANDALONE"
fi

if _contains "$ROOT/zephyr/Kconfig" "config MQTT_MIN_BROKER" &&
   _contains "$ROOT/zephyr/Kconfig" "config MQTT_STANDALONE" &&
   _contains "$ROOT/zephyr/Kconfig" "default n"; then
    _ok "Kconfig exposes module and standalone options"
else
    _fail "Kconfig is missing module or standalone options"
fi

echo ""
echo "=== Zephyr embedder build ==="

if ! command -v west >/dev/null 2>&1; then
    _skip "west not available; skipped actual Zephyr build"
elif [ -z "${ZEPHYR_BASE:-}" ] || [ ! -d "$ZEPHYR_BASE" ]; then
    _skip "ZEPHYR_BASE is not set to a directory; skipped actual Zephyr build"
else
    APP_DIR="$(mktemp -d /tmp/mqtt_min_broker_zephyr_app.XXXXXX)"
    mkdir -p "$APP_DIR/src"

    cat >"$APP_DIR/CMakeLists.txt" <<'EOF_APP_CMAKE'
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt_min_broker_module_smoke)

target_sources(app PRIVATE src/main.c)
EOF_APP_CMAKE

    cat >"$APP_DIR/prj.conf" <<'EOF_APP_CONF'
CONFIG_NETWORKING=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_MQTT_MIN_BROKER=y
CONFIG_MQTT_STANDALONE=n
CONFIG_LOG=y
CONFIG_MAIN_STACK_SIZE=2048
EOF_APP_CONF

    cat >"$APP_DIR/src/main.c" <<'EOF_APP_MAIN'
#include <zephyr/kernel.h>
#include "broker.h"

int main(void)
{
    /*
     * Reference the module API without entering broker_run(). This proves the
     * module can be consumed by an app that owns main() and network bring-up.
     */
    (void)broker_init;
    (void)broker_run;
    return 0;
}
EOF_APP_MAIN

    rm -rf "$BUILD_DIR"
    if west build -b "$BOARD" "$APP_DIR" -d "$BUILD_DIR" -- \
        -DZEPHYR_EXTRA_MODULES="$ROOT" >/tmp/mqtt_zephyr_module_build.log 2>&1; then
        _ok "west build succeeds for CONFIG_MQTT_MIN_BROKER=y, CONFIG_MQTT_STANDALONE=n"
    else
        _fail "west build failed for Zephyr module smoke app"
        sed 's/^/    /' /tmp/mqtt_zephyr_module_build.log
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
