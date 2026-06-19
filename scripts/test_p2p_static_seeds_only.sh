#!/usr/bin/env bash
# P2P smoke test for deployments that disable UDP discovery and rely only on
# configured static seeds.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

STATIC_SEEDS_ONLY=1 \
P2P_TEST_OUT="$SCRIPT_DIR/../build_out/p2p_static_seeds_only_test" \
NODE_A_MQTT=18841 \
NODE_B_MQTT=18842 \
NODE_A_P2P=48941 \
NODE_B_P2P=48942 \
DISCOVERY_PORT=48950 \
    "$SCRIPT_DIR/test_p2p_dynamic.sh"
