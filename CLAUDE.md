# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Minimal MQTT broker implemented in C. Target: MQTT v3.1.1, QoS 0/1, TCP transport. No external dependencies beyond libc.

## Build

```bash
make          # build broker binary
make clean    # remove build artifacts
make test     # run tests (if present)
```

Build system: Makefile. Object files go to `build/`, binary to `build/mqtt_broker` (or adjust once defined).

## Architecture

```
main.c              entry point, event loop, signal handling
server.c / server.h TCP listener, accept loop, fd management
client.c / client.h per-client state machine (CONNECT → active → DISCONNECT)
packet.c / packet.h MQTT packet encode/decode (fixed header, variable header, payload)
topic.c  / topic.h  subscription trie, wildcard matching (+ and #)
session.c / session.h persistent session storage (clean-session flag)
```

Key data flow:
1. `server.c` accepts TCP connections and creates a `client_t` per connection.
2. `client.c` reads raw bytes, hands complete packets to `packet.c` for parsing.
3. Parsed PUBLISH/SUBSCRIBE/UNSUBSCRIBE packets are dispatched through `topic.c`.
4. `topic.c` matches subscribers and calls back into `client.c` to deliver messages.

## MQTT packet framing

Fixed header: 1 byte (type+flags) + variable-length remaining-length (1–4 bytes, MSB continuation bit). Always validate remaining-length before reading payload to avoid overread.

## Session state

A `client_t` tracks: client ID, clean-session flag, keep-alive timer, in-flight QoS 1 messages (packet ID → payload map), and subscription list. QoS 2 is out of scope for the minimal build.

## Coding conventions

Follow the same style as the sibling `io_p2p` module:
- `snake_case` for all identifiers.
- Header guards: `#ifndef MODULE_NAME_H` / `#define MODULE_NAME_H`.
- Expose only what other modules need; keep internal helpers `static`.
- Return 0 on success, negative errno on failure.
- No dynamic allocation in hot paths where a fixed-size pool suffices.
