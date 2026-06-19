# Release Notes

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
