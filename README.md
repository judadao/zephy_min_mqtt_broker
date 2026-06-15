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

---

## Usage as a Zephyr Module (recommended)

Include this repo as a Zephyr module in any existing Zephyr project — no build-system changes needed beyond adding it to `west.yml` and enabling one Kconfig option.

### 1. Add to `west.yml`

```yaml
manifest:
  projects:
    - name: mqtt_min_broker
      url: https://github.com/your-org/mqtt_min_broker
      revision: main
      path: modules/mqtt_min_broker
```

Run `west update` once after editing.

### 2. Enable in `prj.conf`

```
CONFIG_MQTT_MIN_BROKER=y
```

This automatically pulls in `NET_SOCKETS_POSIX_NAMES`. You still need to enable networking in your own `prj.conf`:

```
CONFIG_NETWORKING=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_IPV4=y
```

### 3. Call from your `main.c`

```c
#include "broker.h"

int main(void)
{
    /* bring up your network interface / WiFi here, wait for DHCP */

    broker_init();
    broker_run(); /* does not return */
    return 0;
}
```

No other source files or include paths need to be added to your `CMakeLists.txt`.

---

## Docker Build (zero host setup)

No Zephyr toolchain or SDK required on the host — everything runs inside a container.

### Requirements

- Docker (with `linux/amd64` support; on Apple Silicon use Docker Desktop with Rosetta)

### First run

```bash
./docker-build.sh
```

The first run builds the Docker image (downloads Zephyr v3.7.0 + ESP32 toolchain — allow ~15 min). Subsequent runs skip the image build and go straight to `west build`.

Build artifacts land in `./build/` on the host when done.

### Iterating on source

The source directory is **mounted** into the container (not copied), so you can edit files and rebuild immediately — no need to rebuild the image:

```bash
# edit src/broker.c  (or any source file)
./docker-build.sh   # re-runs west build with your latest changes
```

### Targeting a different board

```bash
BOARD=esp32s3 ./docker-build.sh
```

### Flash (still requires host tools)

```bash
west flash          # run on host after the Docker build produces ./build/
```

---

## Standalone Build (ESP32 app)

Use this when you already have a Zephyr environment set up on the host.

### Requirements

- [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) with `$ZEPHYR_BASE` set
- `west` build tool
- ESP32 board

### Setup & Build

```bash
# source your Zephyr environment
source <workspace>/zephyr/zephyr-env.sh

# verify tools + update west modules
./setup.sh setup

# build
./setup.sh build
# or equivalently:
west build -b esp32 .

# flash
west flash

# open serial monitor
west espressif monitor
```

To tune memory limits, max clients, or log level:

```bash
west build -t menuconfig
```

---

## Configuration

Key values in `prj.conf` (standalone) or your app's `prj.conf` (module):

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

---

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

---

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
