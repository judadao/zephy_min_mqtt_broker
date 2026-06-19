# Release Notes

## minmqtt-v0.1.10 (2026-06-19)

Patch release focused on parser argument validation.

### Included
- Reject NULL arguments in CONNECT and PUBLISH parsers
- Reject NULL packet, output, topic, QoS, and count arguments in SUBSCRIBE parser
- Reject NULL packet, output, topic, and count arguments in UNSUBSCRIBE parser
- Add unit coverage for parser NULL argument handling

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

## minmqtt-v0.1.9 (2026-06-19)

Patch release focused on Remaining Length helper argument validation.

### Included
- Reject NULL inputs in `packet_decode_remaining_len()`
- Reject NULL outputs in `packet_decode_remaining_len()` and `packet_encode_remaining_len()`
- Add unit coverage for invalid Remaining Length helper arguments

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

## minmqtt-v0.1.8 (2026-06-19)

Patch release focused on CONNACK and SUBACK builder strictness.

### Included
- Reject invalid CONNACK return codes
- Reject session-present on refused CONNACK responses
- Reject invalid SUBACK return codes while preserving the failure return code
- Add unit coverage for CONNACK and SUBACK invalid builder inputs

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

## minmqtt-v0.1.7 (2026-06-19)

Patch release focused on defensive packet builder validation.

### Included
- Reject packet_id=0 in SUBACK, UNSUBACK, PUBACK, PUBREC, PUBREL, and PUBCOMP builders
- Reject NULL output pointers in fixed-size packet builders
- Reject SUBACK with zero return codes or NULL return-code array
- Add unit coverage for fixed-size builder invalid inputs

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

## minmqtt-v0.1.6 (2026-06-19)

Patch release focused on defensive PUBLISH frame construction.

### Included
- Reject invalid `packet_build_publish()` inputs before encoding
- Avoid unbounded topic string reads in PUBLISH builder
- Add unit coverage for invalid topic, QoS, packet-id, payload length, and NULL arguments

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

## minmqtt-v0.1.5 (2026-06-19)

Patch release focused on UNSUBSCRIBE topic-filter validation.

### Included
- Close client connections on UNSUBSCRIBE packets with invalid topic filters
- Add integration coverage for empty and malformed UNSUBSCRIBE filters

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_connect_edge.sh`

## minmqtt-v0.1.4 (2026-06-19)

Patch release focused on packet identifier validation for MQTT ACK flows.

### Included
- Close client connections on PUBACK/PUBREC/PUBREL/PUBCOMP with packet_id=0
- Add malformed-packet integration coverage for ACK packet_id=0 cases

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_malformed.sh`

## minmqtt-v0.1.3 (2026-06-19)

Patch release focused on MQTT fixed-header protocol hardening.

### Included
- Close client connections on reserved MQTT packet types 0 and 15
- Add malformed-packet integration coverage for reserved packet types

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_malformed.sh`

## minmqtt-v0.1.2 (2026-06-19)

Patch release focused on CONNECT parser strictness.

### Included
- Reject CONNECT packets with trailing bytes after the declared payload fields
- Add unit coverage for trailing bytes after basic CONNECT and auth CONNECT
- Clean up completed TODO plan items for public API docs and P2P snapshots

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_connect_edge.sh`

## minmqtt-v0.1.1 (2026-06-19)

Patch release focused on MQTT parser hardening, reusable module observability,
and Zephyr/P2P integration checks.

### Included
- Reject malformed SUBSCRIBE requested-QoS bytes with reserved bits or QoS=3
- Reject SUBSCRIBE/UNSUBSCRIBE packets that exceed parser topic array capacity
- Close client connections on malformed SUBSCRIBE/UNSUBSCRIBE parse errors
- Add malformed-packet integration coverage for SUBSCRIBE requested-QoS errors
- Add Zephyr module smoke script for `CONFIG_MQTT_MIN_BROKER=y` with
  `CONFIG_MQTT_STANDALONE=n`
- Add `p2p_peer_snapshot()` for connected P2P peer status
- Add static P2P seed API and `CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY`
- Expand header-level public API documentation

### Validation
- `make -f Makefile.linux unit-tests`
- `./scripts/test_malformed.sh`
- `./scripts/test_p2p_dynamic.sh`
- `make -f Makefile.linux P2P=1 STATIC_SEEDS_ONLY=1 all`

## minmqtt-v0.1.0 (2026-06-19)

First stable module release.

### Included
- MQTT v3.1.1 core: QoS 0/1/2, retained messages, persistent sessions, keepalive
- Zephyr module: CONFIG_MQTT_MIN_BROKER, CONFIG_MQTT_STANDALONE, CONFIG_MQTT_P2P_DYNAMIC
- Optional dynamic P2P broker routing
- Linux test harness and smoke test suite

### API surface
- `broker_init()` — initialise client pool and listen socket
- `broker_run()` — main blocking loop (never returns)
- Tunable via `MQTT_BROKER_PORT` and `MQTT_MAX_CLIENTS` defines

### Deferred
- Zephyr board-specific build smoke test in a configured Zephyr environment
