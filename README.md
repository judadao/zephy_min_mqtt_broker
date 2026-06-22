# mqtt_min_broker

Reusable MQTT v3.1.1 broker module for Linux development and Zephyr/ESP32
embedding.

`mqtt_min_broker` is the broker engine used by Dephy products. It owns MQTT
packet parsing/building, client/session/topic state, retained messages, QoS
paths, optional dashboard support, and optional broker-to-broker P2P routing.
Products pin this module instead of copying broker logic into application code.

## Overview

Use this repo when a product needs an embeddable MQTT broker or a Linux broker
for development and tests. The README covers build, test, optional features, and
module integration without requiring the legacy long document first.

## Key Value

- A compact MQTT broker that can run as a Linux tool or as an embedded module.
- Packet, topic, session, retained-message, and QoS logic with Linux tests.
- Optional HTTP dashboard for inspection during development.
- Optional P2P routing so multiple brokers can exchange messages.
- POSIX and Zephyr platform adapters around the same portable broker core.

## How To Use

1. Build and test on Linux first.
2. Enable optional features such as dashboard or P2P only when needed.
3. In a product repo, pin the module in `deps.json`.
4. Sync it into `deps/` and include public headers from the pinned checkout.
5. Start the broker from product-owned runtime code.

Commands:

```sh
make -f Makefile.linux
make -f Makefile.linux unit-tests
make -f Makefile.linux test
make -f Makefile.linux test-all
make -f Makefile.linux DASHBOARD=1
make -f Makefile.linux P2P=1 test-all
```

## How It Works

The broker core is portable C. Platform adapters provide sockets, timers, and
threading integration. MQTT packets are parsed into bounded internal data
structures, then routed through topic/session state. Retained messages and QoS
state live in the broker layer, while product provisioning and field workflows
remain outside this repo.

P2P mode adds a broker routing layer on top of the same topic model. It is used
when field nodes need to exchange MQTT traffic through peer brokers rather than
only through a single central broker.

## Layout

```text
include/             public broker, packet, topic, session, P2P headers
src/                 broker implementation and standalone entry point
platform/            POSIX and Zephyr platform adapters
scripts/             integration tests, benchmarks, audits
tests/               C unit and helper clients
zephyr/              Zephyr module metadata
docs/                design notes and legacy README
docs/todo.yaml       module TODO source of truth
```

## Benchmarks And Audits

```sh
make -f Makefile.linux packet-buffer-audit
make -f Makefile.linux testkit-wrapper
scripts/bench_p2p_scale.sh
scripts/bench_p2p_docker_scale.sh
```

## More Docs

- `docs/readme_legacy.md`: previous long README with detailed feature, API, and
  benchmark notes.
- `docs/module_structure.md`: current module structure contract.
- `docs/product_dependency_model.md`: recommended product dependency flow.
- `docs/repository_split_plan.md`: product/module ownership split.
- `docs/release_notes.md`: release history.
