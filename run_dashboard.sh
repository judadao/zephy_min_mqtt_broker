#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

need()
{
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'error: %s is required\n' "$1" >&2
        exit 1
    fi
}

port_in_use()
{
    port=$1
    if command -v ss >/dev/null 2>&1; then
        ss -tlnH "sport = :$port" 2>/dev/null | grep -q .
    elif command -v lsof >/dev/null 2>&1; then
        lsof -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1
    else
        return 1
    fi
}

need make
need gcc

if port_in_use 8080; then
    printf 'error: HTTP dashboard port 8080 is already in use\n' >&2
    exit 1
fi

printf 'Building MQTT broker with Linux dashboard...\n'
make -C "$ROOT_DIR" -f Makefile.linux DASHBOARD=1 >/dev/null

printf '\nStarting MQTT broker dashboard...\n'
printf 'Open: http://127.0.0.1:8080/\n'
printf 'MQTT port: 1883\n\n'

exec "$ROOT_DIR/build_out/mqtt_broker"
