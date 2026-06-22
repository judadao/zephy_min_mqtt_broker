# mqtt_min_broker Module Structure

`mqtt_min_broker` is a reusable MQTT v3.1.1 broker module for Linux validation
and Zephyr/ESP32 embedding. Product applications should consume it through a
pinned dependency and call the public broker APIs instead of copying broker
logic into product code.

## Public Headers

- `include/broker.h`: broker lifecycle and runtime configuration.
- `include/client.h`: client connection handling boundaries.
- `include/packet.h`: MQTT packet encode/decode helpers.
- `include/topic.h`: local topic routing, retained messages, and subscriptions.
- `include/session.h`: persistent session and inflight message state.
- `include/p2p.h` and `include/p2p_shard.h`: optional dynamic P2P routing APIs.
- `include/http.h`: optional Linux dashboard integration.
- `include/wifi.h`: platform WiFi abstraction hooks.

## Source Layout

- `src/broker.c`: broker lifecycle and accept loop integration.
- `src/client.c`: MQTT client state machine.
- `src/packet.c`: MQTT v3.1.1 packet parsing and builders.
- `src/topic.c`: subscription matching, retained messages, and publish fan-out.
- `src/session.c`: offline queues and QoS inflight handling.
- `src/p2p_*.c`: discovery, election, peer links, routing, and shard ownership.
- `src/http.c`: optional dashboard/status server.
- `src/main.c`: standalone Linux/demo entry point.

## Platform And Build Metadata

- `platform/posix/`: Linux sockets, mutexes, time, and thread adapters.
- `platform/zephyr/`: Zephyr sockets, synchronization, and logging adapters.
- `zephyr/`: Zephyr module metadata.
- `Makefile.linux`: Linux build, tests, and feature-flag entry point.
- `CMakeLists.txt` and `Kconfig`: module integration for Zephyr consumers.

## Validation

- `tests/unit_*.c`: focused C unit coverage for packet, topic, and session
  behavior.
- `scripts/test_*.sh`: integration coverage for broker behavior, QoS,
  malformed packets, auth, P2P, dashboard, and Zephyr metadata.
- `scripts/bench_*.sh`: repeatable Linux benchmark entry points.

## Integration Rule

Reusable MQTT and P2P behavior belongs in this module. Product-specific
provisioning, UI, deployment flows, and field scenarios belong in product repos
that pin a released broker tag.
