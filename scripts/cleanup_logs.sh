#!/usr/bin/env bash
# Remove old log files and keep only the newest N.
#
# Usage:
#   ./scripts/cleanup_logs.sh
#   KEEP=5 LOG_ROOT=build_out ./scripts/cleanup_logs.sh
set -uo pipefail

KEEP="${KEEP:-5}"
LOG_ROOT="${LOG_ROOT:-build_out}"

if ! [[ "$KEEP" =~ ^[0-9]+$ ]] || [ "$KEEP" -lt 0 ]; then
    echo "[fail] KEEP must be a non-negative integer" >&2
    exit 1
fi

if [ ! -d "$LOG_ROOT" ]; then
    echo "[skip] log root does not exist: $LOG_ROOT" >&2
    exit 0
fi

while IFS= read -r file; do
    [ -n "$file" ] || continue
    rm -f "$file"
done < <(
    find "$LOG_ROOT" -type f -name '*.log' -printf '%T@ %p\n' 2>/dev/null \
        | sort -nr \
        | awk -v keep="$KEEP" 'NR > keep {sub(/^[^ ]+[[:space:]]+/, ""); print}'
)
