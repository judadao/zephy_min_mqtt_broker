# Release Notes

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

### Deferred to v0.2.0
- Static P2P seed API
- P2P peer status snapshot API
- P2P route/remote subscription stats API
- Zephyr board-specific build smoke test
