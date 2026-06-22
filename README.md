# mqtt_min_broker

Reusable MQTT v3.1.1 broker module for Linux development and Zephyr/ESP32
embedding.

This repo owns broker behavior: packet parsing/building, client/session/topic
state, retained messages, QoS paths, optional dashboard, and optional P2P
broker routing. Product provisioning and field workflows belong in product
repos that pin this module.

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

## Quick Commands

```sh
make -f Makefile.linux
make -f Makefile.linux unit-tests
make -f Makefile.linux test
make -f Makefile.linux packet-buffer-audit
make -f Makefile.linux testkit-wrapper
make -f Makefile.linux P2P=1 test-all
```

Benchmarks:

```sh
scripts/bench_p2p_scale.sh
scripts/bench_p2p_docker_scale.sh
```

## Module Use

Product repos should pin this module in `deps.json`, sync it into `deps/`, and
include public headers from the pinned checkout. Do not copy broker logic into
product app code.

## More Docs

- `docs/readme_legacy.md`: previous long README with detailed feature, API, and
  benchmark notes.
- `docs/module_structure.md`: current module structure contract.
- `docs/product_dependency_model.md`: recommended product dependency flow.
- `docs/repository_split_plan.md`: product/module ownership split.
- `docs/release_notes.md`: release history.
