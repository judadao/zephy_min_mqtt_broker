# mqtt_min_broker

Reusable MQTT v3.1.1 broker module for Linux development and Zephyr/ESP32
embedding.

## Overview

`mqtt_min_broker` is the reusable broker engine used by Dephy products. It can
run as a Linux broker for development/tests or as an embedded module in Zephyr
products.

## Key Value

- MQTT packet, topic, session, retained-message, and QoS logic.
- POSIX and Zephyr adapters around one portable C core.
- Optional dashboard for development inspection.
- Optional P2P broker routing for field-node message exchange.

## How To Use

```sh
make -f Makefile.linux
make -f Makefile.linux test
make -f Makefile.linux test-all
make -f Makefile.linux DASHBOARD=1
make -f Makefile.linux P2P=1 test-all
```

Products should pin this module in `deps.json`, sync it into `deps/`, include
public headers, and start the broker from product-owned runtime code.

## Simple Principle

The broker core owns MQTT behavior. Platform adapters provide OS integration.
Products own provisioning and field workflows.

## Docs

- `docs/readme_legacy.md`: previous long README with detailed notes.
- `docs/module_structure.md`: current module structure contract.
- `docs/product_dependency_model.md`: product dependency flow.
- `docs/release_notes.md`: release history.
- `docs/todo.md`: current TODO summary.
