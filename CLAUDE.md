# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Minimal MQTT broker implemented in C. Target: MQTT v3.1.1, QoS 0/1, TCP transport. No external dependencies beyond libc.

## Build (Zephyr / west)

```bash
west build -b esp32 .          # build for ESP32
west flash                     # flash to device
west build -t menuconfig       # open Kconfig menu
```

Build system: CMake + Zephyr (`CMakeLists.txt` + `prj.conf`). Requires a configured Zephyr workspace with `$ZEPHYR_BASE` set.

## Architecture

```
src/main.c      entry point; calls client_pool_init() → broker_init() → broker_run()
src/broker.c    TCP listen socket; accept loop; calls client_alloc() per connection
src/client.c    per-client Zephyr thread (one thread per connection from client_stacks[]);
                MQTT state machine: dispatches CONNECT/PUBLISH/SUBSCRIBE/etc.
src/packet.c    MQTT v3.1.1 encode/decode (no dynamic alloc)
src/topic.c     flat subscription table (TOPIC_MAX_SUBS entries); wildcard matching (+ #);
                retain message store; fan-out under topic_lock mutex
src/session.c   persistent session store (survives reconnect when clean_session=0)
```

Key data flow:
1. `broker_run()` accepts TCP fds and calls `client_alloc()`.
2. `client_alloc()` picks a free slot from the static pool and spawns a thread from `client_stacks[]`.
3. The client thread calls `recv_packet()` in a loop (blocking recv), then dispatches by packet type.
4. PUBLISH → `topic_publish()` → iterates `subs[]`, calls `client_send()` on each match.

## Concurrency model

- One Zephyr thread per connected client; stack size = `CLIENT_STACK_SIZE` (2048 bytes).
- `pool_lock` mutex guards the client slot array in `client.c`.
- `topic_lock` mutex guards `subs[]` and `retains[]` in `topic.c`.
- `session_lock` mutex guards `sessions[]` in `session.c`.
- All mutexes are `K_MUTEX_DEFINE` (static, no init call needed).

## MQTT packet framing

Fixed header: 1 byte (type|flags) + variable-length remaining-length (1–4 bytes, MSB = continuation bit). `recv_packet()` in `client.c` reads byte-by-byte until the continuation bit clears, then reads the full payload in one `recv_exact()` call.

## Scope / known omissions

- MQTT v3.1.1 only; QoS 0 and 1 (QoS 2 not implemented).
- No TLS. No username/password auth (hooks present in `handle_connect`, marked TODO).
- WiFi init must be done before `broker_init()`; add it in `main.c` (marked TODO).
- Keepalive timeout enforcement not yet implemented (last_seen_ms tracked, timer missing).

## Coding conventions

Follow the same style as the sibling `io_p2p` module:
- `snake_case` for all identifiers.
- Header guards: `#ifndef MODULE_NAME_H` / `#define MODULE_NAME_H`.
- Expose only what other modules need; keep internal helpers `static`.
- Return 0 on success, negative errno on failure.
- No dynamic allocation; all pools are statically sized arrays.
