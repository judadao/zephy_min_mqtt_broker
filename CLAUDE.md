# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Minimal MQTT v3.1.1 broker implemented in C. It supports Linux development/testing and Zephyr/ESP32 deployment. Core features include QoS 0/1/2, retained messages, persistent sessions, keepalive enforcement, optional username/password auth, optional Linux HTTP dashboard, and optional dynamic broker P2P routing.

## Build

Linux:

```bash
make -f Makefile.linux
make -f Makefile.linux DASHBOARD=1
make -f Makefile.linux AUTH_USER=admin AUTH_PASS=secret
make -f Makefile.linux P2P=1
```

Zephyr:

```bash
west build -b esp32 .
west flash
west build -t menuconfig
```

Docker Zephyr build:

```bash
./docker-build.sh
BOARD=esp32s3 ./docker-build.sh
```

## Tests

```bash
./scripts/run_all_tests.sh          # run every suite; pass --all to include slow tests
./scripts/run_all_tests.sh --all    # includes dashboard and keepalive timing tests
```

Individual suites:

```bash
./scripts/test_broker.sh            # basic MQTT smoke suite (pub/sub, QoS 0/1/2, retain)
./scripts/test_auth.sh              # username/password auth (CONNACK 0x04 on bad creds)
./scripts/test_keepalive.sh         # keepalive enforcement (1.5× rule, §3.1.2.10)
./scripts/test_lwt.sh               # Last Will and Testament delivery
./scripts/test_qos2.sh              # QoS 2 PUBLISH→PUBREC→PUBREL→PUBCOMP exchange
./scripts/test_session.sh           # persistent session offline delivery (QoS 1)
./scripts/test_session_qos2.sh      # persistent session offline delivery (QoS 2)
./scripts/test_inflight_retry.sh    # QoS 1 DUP retransmit on reconnect
./scripts/test_connect_edge.sh      # CONNECT edge cases: client-ID takeover, PINGREQ
./scripts/test_malformed.sh         # malformed/truncated packets — broker must not crash
./scripts/test_large_payload.sh     # near-max payload delivery and boundary checks
./scripts/test_unsub.sh             # UNSUBSCRIBE stops delivery, others unaffected
./scripts/test_stress.sh            # concurrent clients and pool exhaustion
./scripts/test_connack_unavail.sh   # CONNACK 0x03 when client pool is full (§3.2.2.3)
./scripts/test_dashboard.sh         # HTTP dashboard REST API (requires DASHBOARD=1 build)
./scripts/test_p2p_dynamic.sh       # two local P2P brokers and cross-node publish
./scripts/bench_p2p_scale.sh        # P2P scale benchmark: N brokers, throughput sweep
```

The P2P test compiles two broker binaries with different MQTT/P2P ports and uses `MQTT_P2P_PEERS` as a deterministic local seed.

## Architecture

```
src/main.c       entry point; init client pool, broker, optional HTTP/P2P
src/broker.c     TCP listen socket; accept loop; calls client_alloc()
src/client.c     one thread per client; MQTT state machine and QoS handling
src/packet.c     MQTT v3.1.1 encode/decode; no dynamic allocation
src/topic.c      local subscription table, wildcard match, retain store, fan-out
src/session.c    persistent session store
src/http.c       optional Linux dashboard + REST API
src/p2p_*.c      optional discovery, election, peer transport, remote routing
platform/*       POSIX and Zephyr abstraction layer
```

PUBLISH flow without P2P:

```text
client thread -> recv_packet() -> handle_publish() -> topic_publish()
  -> local topic_match() fan-out -> session_offline_publish()
```

PUBLISH flow with `CONFIG_MQTT_P2P_DYNAMIC`:

```text
topic_publish() -> local fan-out -> p2p_publish_from_local()
ROUTER nodes forward by remote subscription table
remote node -> topic_publish_remote() -> local fan-out only
```

## Dynamic Broker Notes

Dynamic mode is opt-in. Nodes announce score over UDP discovery, connect over TCP port `4884`, exchange HELLO/SUB/UNSUB/PUBLISH frames, and use `(origin_id, seq)` seen-cache entries to prevent loops. `MQTT_P2P_PEERS=ip:port[,ip:port]` can seed peer connections when UDP broadcast is unavailable or for same-host tests.

## Concurrency Model

- One thread per MQTT client.
- P2P adds discovery announce/listen threads, peer accept/connect threads, and one thread per P2P peer.
- `pool_lock`, `topic_lock`, `session_lock`, P2P peer/election/router locks guard shared static tables.
- Avoid dynamic allocation; keep buffers and tables statically bounded.

## Coding Conventions

- Use `snake_case` identifiers and `UPPER_SNAKE_CASE` macros.
- Keep internal helpers `static`.
- Return `0` on success and negative errno-style values on failures where practical.
- Keep Linux and Zephyr paths compiling; use `platform/platform.h` abstractions for sockets, mutexes, time, and logging.
