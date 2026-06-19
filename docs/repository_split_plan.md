# Repository Split Plan

Recommended direction: use two repositories.

1. `mqtt_min_broker`
   - reusable MQTT broker module
   - Linux development and test harness
   - Zephyr module integration
   - P2P broker routing core
   - versioned by Git tags using `minmqtt-vX.Y.Z`

2. Product application repository
   - Note 1 / Note 2 / Note 3 field bridge app
   - local HTML provisioning and operations UI
   - WiFi setup UX
   - bridge peer setup UX
   - product-specific 4510 topic workflow
   - `deps.json` pins a specific `mqtt_min_broker` tag under `deps/`
   - versioned by Git tags using `bridge-vX.Y.Z`

This keeps the broker reusable for other Zephyr applications while allowing the
product app to move faster without turning product UI and provisioning code into
broker module requirements.

## mqtt_min_broker repo scope

Keep in this repository:

- MQTT v3.1.1 packet handling
- client/session/topic/retained-message logic
- QoS behavior
- broker public API
- platform abstraction
- Zephyr module files:
  - `zephyr/module.yml`
  - `zephyr/Kconfig`
  - `zephyr/CMakeLists.txt`
- optional P2P broker routing module
- Linux tools and smoke tests
- module API documentation
- release tags and release notes

Avoid adding product-only code here:

- Note 1 / Note 2 / Note 3 field workflow screens
- product HTML provisioning UI
- product-specific WiFi setup wizard
- 4510-only topic pages
- product `deps/` checkout
- product build scripts that pin this repo as a dependency

Allowed exception: small examples or test apps that prove module integration.

## Product app repo scope

Suggested repo name:

```text
mqtt_field_bridge_app
```

Suggested layout:

```text
mqtt_field_bridge_app/
├── deps.json
├── deps/
│   └── mqtt_min_broker/
├── app/
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── boards/
│   └── src/
│       ├── main.c
│       ├── product_config.c
│       ├── product_config.h
│       ├── provisioning_http.c
│       ├── provisioning_http.h
│       ├── bridge_control.c
│       └── bridge_control.h
├── web/
│   ├── index.html
│   ├── app.css
│   └── app.js
├── scripts/
│   ├── sync_deps.sh
│   ├── build_product.sh
│   └── flash_product.sh
└── docs/
    ├── field_bridge_scenario.md
    └── operations_guide.md
```

The product app owns:

- `main()`
- WiFi provisioning
- broker start/stop policy
- static peer configuration
- local HTML UI
- product config persistence
- field diagnostics

The product app consumes `mqtt_min_broker` through:

```text
deps/mqtt_min_broker/
```

## Dependency flow

Normal product work:

1. Product repo reads `deps.json`.
2. `scripts/sync_deps.sh` checks out `deps/mqtt_min_broker` at the pinned tag.
3. Product build uses that fixed module copy.
4. Product code changes stay in the product repo.

When broker behavior needs a fix:

1. Reproduce or describe the issue against `mqtt_min_broker`.
2. Fix it in `mqtt_min_broker`.
3. Run broker tests.
4. Tag a new broker version.
5. Update product `deps.json` to the new tag.
6. Rebuild and validate the product app.

## Initial TODO

### mqtt_min_broker

- Confirm `CONFIG_MQTT_MIN_BROKER=y` builds as a Zephyr module without
  `CONFIG_MQTT_STANDALONE`.
- Document the minimal module API and lifecycle.
- Add release notes file for module tags.
- Add a first stable tag once module API boundaries are confirmed.
- Prefer broker tags such as `minmqtt-v0.1.0`.
- Keep product-specific provisioning out of the broker core.

### mqtt_field_bridge_app

- Create new product repository.
- Add `deps.json` with pinned `mqtt_min_broker` tag.
- Use branch `product/mqtt-bridge` for initial product work.
- Use product release tags such as `bridge-v0.1.0`.
- Add `scripts/sync_deps.sh`.
- Add product `main.c` that initializes network, broker, and P2P.
- Add product config model for WiFi and bridge peers.
- Add local HTML provisioning UI.
- Add REST API for status, WiFi, broker control, and peers.
- Add 3-node field validation script/checklist.
