#!/usr/bin/env bash
# Run every test suite and report a combined pass/fail summary.
# Skips time-sensitive tests by default (pass --all to include them).
#
# Usage:
#   ./scripts/run_all_tests.sh          # fast suites only (~30s)
#   ./scripts/run_all_tests.sh --all    # include slow suites (~60s extra)
#
# Exit code: 0 if all suites pass, 1 otherwise.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
PASS=0; FAIL=0; SKIP=0
RUN_ALL=0

for arg in "$@"; do
    [ "$arg" = "--all" ] && RUN_ALL=1
done

_ok()   { echo "  [SUITE PASS] $1"; PASS=$((PASS+1)); }
_fail() { echo "  [SUITE FAIL] $1 (exit $2)"; FAIL=$((FAIL+1)); }
_skip() { echo "  [SUITE SKIP] $1 — $2"; SKIP=$((SKIP+1)); }

_port_in_use() { ss -tlnH "sport = :1883" 2>/dev/null | grep -q .; }

# ── build everything first ────────────────────────────────────────────────────
echo "=== Building all targets ==="
if ! make -f "$ROOT/Makefile.linux" -C "$ROOT" all test-helpers -s; then
    echo "[ERROR] build failed — aborting" >&2
    exit 1
fi
echo ""

# ── unit tests ────────────────────────────────────────────────────────────────
echo "=== Unit tests ==="
if make -f "$ROOT/Makefile.linux" -C "$ROOT" unit-tests 2>&1 | \
       grep -E "Results:|FAIL"; then
    :
fi
make -f "$ROOT/Makefile.linux" -C "$ROOT" unit-tests -s 2>/dev/null
UNIT_RC=$?
if [ $UNIT_RC -eq 0 ]; then
    _ok "unit tests"
else
    _fail "unit tests" $UNIT_RC
fi
echo ""

# ── integration suites ────────────────────────────────────────────────────────
# Each suite starts/stops its own broker; they must not overlap.
# Any suite that needs port 1883 will fail if it's in use.

run_suite() {
    local name="$1" script="$2"
    if _port_in_use; then
        echo "[WARNING] port 1883 in use before '$name' — waiting 1s"
        sleep 1
    fi
    echo "=== $name ==="
    if bash "$script" 2>&1; then
        _ok "$name"
    else
        _fail "$name" $?
    fi
    echo ""
    sleep 0.3   # brief drain between suites
}

run_suite "smoke (test_broker)"          "$SCRIPT_DIR/test_broker.sh"
run_suite "LWT"                          "$SCRIPT_DIR/test_lwt.sh"
run_suite "session persistence"          "$SCRIPT_DIR/test_session.sh"
run_suite "QoS 2"                        "$SCRIPT_DIR/test_qos2.sh"
run_suite "session + QoS 2"             "$SCRIPT_DIR/test_session_qos2.sh"
run_suite "UNSUBSCRIBE"                  "$SCRIPT_DIR/test_unsub.sh"
run_suite "connection edge cases"        "$SCRIPT_DIR/test_connect_edge.sh"
run_suite "CONNACK server unavailable"   "$SCRIPT_DIR/test_connack_unavail.sh"
run_suite "malformed packets"            "$SCRIPT_DIR/test_malformed.sh"
run_suite "large payloads"               "$SCRIPT_DIR/test_large_payload.sh"
run_suite "auth (username/password)"     "$SCRIPT_DIR/test_auth.sh"
run_suite "stress"                       "$SCRIPT_DIR/test_stress.sh"
run_suite "Zephyr module smoke"          "$SCRIPT_DIR/test_zephyr_module.sh"
run_suite "compliance (§3.9.3/§3.3.1.3/§3.1.2.4/§4.7.2)" \
                                         "$SCRIPT_DIR/test_compliance.sh"

# Slow suites (keepalive needs ~20s, inflight retry ~20s)
if [ $RUN_ALL -eq 1 ]; then
    run_suite "keepalive enforcement"    "$SCRIPT_DIR/test_keepalive.sh"
    run_suite "QoS1 in-flight retry"     "$SCRIPT_DIR/test_inflight_retry.sh"
else
    _skip "keepalive enforcement" "pass --all to include (~20s)"
    _skip "QoS1 in-flight retry"  "pass --all to include (~20s)"
fi

# P2P test uses separate ports — safe to run alongside others
# but it can be flaky on heavily loaded machines
if [ $RUN_ALL -eq 1 ]; then
    run_suite "P2P dynamic routing"      "$SCRIPT_DIR/test_p2p_dynamic.sh"
else
    _skip "P2P dynamic routing" "pass --all to include"
fi

# Dashboard test requires DASHBOARD=1 build and port 8080
if [ $RUN_ALL -eq 1 ]; then
    if ! ss -tlnH "sport = :8080" 2>/dev/null | grep -q .; then
        run_suite "HTTP dashboard (DASHBOARD=1)" "$SCRIPT_DIR/test_dashboard.sh"
    else
        _skip "HTTP dashboard" "port 8080 in use"
    fi
else
    _skip "HTTP dashboard" "pass --all to include"
fi

# ── summary ───────────────────────────────────────────────────────────────────
echo "════════════════════════════════════════════════"
echo "  Suites passed : $PASS"
echo "  Suites failed : $FAIL"
echo "  Suites skipped: $SKIP"
echo "════════════════════════════════════════════════"

[ $FAIL -eq 0 ]
