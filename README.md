# mqtt_min_broker

A minimal MQTT v3.1.1 broker written in C for [Zephyr RTOS](https://zephyrproject.org/), targeting the ESP32. Designed as a lightweight, Mosquitto-compatible alternative that runs entirely on-device — no Linux host required.

## Features

- MQTT v3.1.1 compliant
- QoS 0 and QoS 1
- Topic wildcard matching (`+` single-level, `#` multi-level)
- Retain message store
- Persistent sessions (`clean_session = 0`)
- Up to 8 concurrent clients
- Zero heap allocation — all pools are static arrays

## Not implemented (minimal scope)

- QoS 2
- TLS / SSL
- Username / password authentication
- Keepalive timeout enforcement (tracked, timer not wired up)
- WebSocket transport

## Requirements

- [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) with `$ZEPHYR_BASE` set
- `west` build tool
- ESP32 board

## Build & Flash

```bash
# one-time workspace init (if not already done)
west init -m https://github.com/zephyrproject-rtos/zephyr
west update

# build
west build -b esp32 /path/to/mqtt_min_broker

# flash
west flash

# open serial monitor
west espressif monitor
```

To tune memory limits, max clients, or log level:

```bash
west build -t menuconfig
```

## Configuration

Key values in `prj.conf`:

| Config | Default | Description |
|--------|---------|-------------|
| `CONFIG_HEAP_MEM_POOL_SIZE` | 65536 | Total heap available to Zephyr |
| `CONFIG_MAIN_STACK_SIZE` | 4096 | Main thread stack (broker accept loop) |

Key values in `include/broker.h`:

| Define | Default | Description |
|--------|---------|-------------|
| `MQTT_BROKER_PORT` | 1883 | TCP port the broker listens on |
| `MQTT_MAX_CLIENTS` | 8 | Maximum concurrent connections |

Key values in `include/client.h`:

| Define | Default | Description |
|--------|---------|-------------|
| `CLIENT_STACK_SIZE` | 2048 | Per-client thread stack (bytes) |

Key values in `include/packet.h`:

| Define | Default | Description |
|--------|---------|-------------|
| `MQTT_TOPIC_MAX` | 128 | Max topic string length |
| `MQTT_PAYLOAD_MAX` | 1024 | Max publish payload size |

## Architecture

```
main.c      WiFi init (TODO) → client_pool_init → broker_init → broker_run
broker.c    TCP listen socket, accept loop, calls client_alloc() per fd
client.c    One Zephyr thread per client; blocking recv loop; MQTT state machine
packet.c    MQTT v3.1.1 packet encode / decode
topic.c     Flat subscription table; wildcard matching; retain store; fan-out
session.c   Persistent session table (survives reconnect when clean_session=0)
```

Data flow for a PUBLISH:

```
client thread → recv_packet() → handle_publish()
    → topic_publish()
        → topic_match() for each subscription
            → client_send() to each matching subscriber
```

Each client runs in its own Zephyr thread (stack from static `client_stacks[]`). Shared state (`subs[]`, `retains[]`, `sessions[]`) is protected by per-module `K_MUTEX_DEFINE` mutexes.

## WiFi Setup

WiFi connection must be established before `broker_init()` is called. Add your WiFi credentials and connection logic to `src/main.c` where the TODO comment is:

```c
/* TODO: init WiFi and wait for DHCP before calling broker_init() */
```

Refer to the [Zephyr WiFi sample](https://docs.zephyrproject.org/latest/samples/net/wifi/README.html) for a connection snippet.

## Testing with mosquitto_pub / mosquitto_sub

Once the broker is running and you know the ESP32's IP address:

```bash
# subscribe
mosquitto_sub -h <ESP32_IP> -t "test/#" -v

# publish
mosquitto_pub -h <ESP32_IP> -t "test/hello" -m "world"

# QoS 1
mosquitto_pub -h <ESP32_IP> -t "sensor/temp" -m "25.3" -q 1
```
