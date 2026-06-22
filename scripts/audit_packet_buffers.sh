#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

echo "packet buffer audit"
rg -n "MQTT_MAX_PACKET_SIZE|MQTT_PAYLOAD_MAX|MQTT_TOPIC_MAX|char [A-Za-z0-9_]+\\[[0-9A-Z_]+\\]|uint8_t [A-Za-z0-9_]+\\[[0-9A-Z_]+\\]" \
    "$root/include" "$root/src" "$root/tests" |
    sed "s#^$root/##" |
    tee "$root/build_out/packet_buffer_audit.txt"

grep -q "MQTT_MAX_PACKET_SIZE" "$root/build_out/packet_buffer_audit.txt"
grep -q "MQTT_PAYLOAD_MAX" "$root/build_out/packet_buffer_audit.txt"
grep -q "MQTT_TOPIC_MAX" "$root/build_out/packet_buffer_audit.txt"

echo "packet buffer audit OK"
