# Release Notes

## minmqtt-v0.1.18 (2026-06-19)

Patch release focused on dashboard request-body stability.

### Included
- Read dashboard POST bodies with an explicit loop until `Content-Length` bytes
  are received
- Return HTTP 400 for oversized or truncated dashboard publish requests instead
  of parsing partial JSON

### Validation
- `make -B -f Makefile.linux DASHBOARD=1 all`
- `./scripts/test_dashboard.sh`

## minmqtt-v0.1.17 (2026-06-19)

Patch release focused on connection rejection robustness.

### Included
- Send pool-full `CONNACK_SERVER_UNAVAIL` responses with a send-all loop so
  partial socket writes do not truncate the rejection frame

### Validation
- `make -B -f Makefile.linux all test-helpers`
- `./scripts/test_connack_unavail.sh`
- `./scripts/test_stress.sh`

## minmqtt-v0.1.16 (2026-06-19)

Patch release focused on topic fan-out contention.

### Included
- Move QoS 1/2 topic fan-out socket writes outside `topic_lock`
- Keep duplicate-subscription de-duplication and packet-id allocation under the
  topic lock, then build/send each frame after releasing it
- Preserve QoS downgrade behavior by only assigning packet identifiers and
  inflight entries for deliveries whose effective QoS is greater than zero

### Validation
- `make -B -f Makefile.linux all test-helpers`
- `make -f Makefile.linux unit-tests`
- `./scripts/test_qos2.sh`
- `./scripts/test_session_qos2.sh`
- `./scripts/test_broker.sh`

## minmqtt-v0.1.15 (2026-06-19)

Patch release focused on optional dashboard stability.

### Included
- Send HTTP dashboard responses with a send-all loop so partial socket writes do
  not truncate JSON or HTML responses
- Validate dashboard `listen()` and POSIX server-thread startup errors
- Log dashboard startup failure from `main()` while keeping the MQTT broker
  available

### Validation
- `make -B -f Makefile.linux DASHBOARD=1 all`
- `./scripts/test_dashboard.sh`

## minmqtt-v0.1.14 (2026-06-19)

Patch release focused on P2P startup stability.

### Included
- Check POSIX P2P discovery announce/listen `pthread_create()` results and log
  startup failures
- Check POSIX P2P TCP accept/connect `pthread_create()` results and log startup
  failures
- Detach long-running POSIX P2P background threads after successful startup

### Validation
- `make -B -f Makefile.linux P2P=1 all`
- `make -B -f Makefile.linux P2P=1 STATIC_SEEDS_ONLY=1 all`
- `./scripts/test_p2p_dynamic.sh`
- `./scripts/test_p2p_static_seeds_only.sh`

## minmqtt-v0.1.13 (2026-06-19)

Patch release focused on static-seed P2P stability.

### Included
- Make `CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY` skip UDP discovery sockets and
  discovery threads entirely
- Keep static-seed-only mode on the existing TCP peer transport and router path
- Detach POSIX P2P peer worker threads so repeated reconnects release thread
  resources after disconnect
- Bound P2P topic/filter string reads before building publish and subscription
  propagation messages
- Send P2P publish floods and subscription fanout from a peer-id snapshot so
  socket writes do not hold the peer table lock
- Snapshot P2P HELLO fanout targets and lock peer-count reads in the connect
  loop to reduce peer table contention and data races
- Parse POSIX `MQTT_P2P_PEERS` only once into the static seed table instead of
  reparsing the environment every connect tick
- Send MQTT client frames with a loop so partial socket writes do not disconnect
  otherwise healthy clients under load
- Report POSIX client worker thread creation failures so failed allocations free
  their client slot and follow the normal server-unavailable path
- Remove unused per-client receive buffer fields from `client_t`, reducing the
  fixed client pool footprint by about `MQTT_MAX_PACKET_SIZE + 8 + sizeof(size_t)`
  per slot
- Give dynamic and static-seed-only P2P smoke tests separate output directories
  so they can run without overwriting each other's test binaries and logs
- Make Linux broker/CLI targets depend on public and platform headers so header
  changes rebuild the binaries used by tests
- Complete the QoS 2 publish handshake in `mqtt_cli pub -q 2` before sending
  DISCONNECT, removing retained-QoS2 test timing dependence
- Assign packet identifiers and inflight tracking for retained QoS 1/2 delivery
  so retained messages are not dropped by strict PUBLISH builder validation
- Add static-seed-only P2P smoke coverage

### Validation
- `make -B -f Makefile.linux all test-helpers`
- `make -f Makefile.linux unit-tests`
- `./scripts/run_all_tests.sh`
- `./scripts/test_p2p_dynamic.sh`
- `./scripts/test_p2p_static_seeds_only.sh`

## minmqtt-v0.1.12 (2026-06-19)

Patch release focused on topic-list parser strictness.

### Included
- Reject SUBSCRIBE packets with no topic filters in `packet_parse_subscribe()`
- Reject UNSUBSCRIBE packets with no topic filters in `packet_parse_unsubscribe()`
- Add unit coverage for empty SUBSCRIBE and UNSUBSCRIBE payloads

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_malformed.sh && ./scripts/test_broker.sh`

## minmqtt-v0.1.11 (2026-06-19)

Patch release focused on parser-level protocol value validation.

### Included
- Reject PUBLISH packets with reserved QoS=3 in `packet_parse_publish()`
- Reject QoS PUBLISH packets with packet_id=0 in `packet_parse_publish()`
- Reject SUBSCRIBE and UNSUBSCRIBE packets with packet_id=0 in their parsers
- Add unit coverage for parser-level packet identifier and QoS validation

### Validation
- `make -f Makefile.linux unit-tests`
- `make -f Makefile.linux all test-helpers && ./scripts/test_broker.sh`

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
