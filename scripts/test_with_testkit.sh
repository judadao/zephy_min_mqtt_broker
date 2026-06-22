#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
testkit="${DEPHY_TESTKIT_ROOT:-$root/../dephy_testkit}"

if [ ! -f "$testkit/scripts/assert.sh" ] || [ ! -x "$testkit/scripts/run_with_result.sh" ]; then
    echo "missing dephy_testkit at $testkit" >&2
    exit 1
fi

. "$testkit/scripts/assert.sh"

out=$("$testkit/scripts/run_with_result.sh" mqtt_min_broker_unit \
    make -f "$root/Makefile.linux" unit-tests)
printf '%s\n' "$out"
assert_contains "$out" '"result":"pass"' "testkit result wrapper reports pass"
