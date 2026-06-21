# Product Dependency Model

This project should use two repositories:

1. `mqtt_min_broker`: the reusable broker module, versioned by Git tags.
2. Product application: the field-oriented app repo that consumes a fixed
   broker module version from `deps/`.

The product application should build against a pinned broker version by default.
If a broker issue is found during product work, file or track the issue against
`mqtt_min_broker`, fix it in the module, tag a new module version, then update
the product `deps.json`.

## Product repo layout

Recommended product-side layout, in a separate product repository:

```text
product_app/
├── deps.json
├── deps/
│   └── mqtt_min_broker/
├── app/
│   ├── CMakeLists.txt
│   ├── prj.conf
│   └── src/
└── scripts/
    ├── sync_deps.sh
    └── build_product.sh
```

`deps/` is the checked-out dependency workspace. The product build should use
the copy under `deps/mqtt_min_broker`, not whatever happens to be installed on
the developer machine.

## Product deps.json

Recommended product-side `deps.json` shape:

```json
{
  "deps": {
    "mqtt_min_broker": {
      "repo": "git@example.com:mqtt_min_broker.git",
      "version": "minmqtt-v0.1.12",
      "path": "deps/mqtt_min_broker"
    }
  },
  "zephyr": {
    "min_version": "v3.5.0"
  },
  "build": {
    "board": "esp32_devkitc/esp32/procpu"
  }
}
```

Rules:

- `version` must point to a Git tag, not a floating branch.
- Product builds should be reproducible from `deps.json`.
- Updating the broker module in the product requires an explicit `deps.json`
  change.
- Product work should not patch files directly under `deps/mqtt_min_broker`
  unless it is a temporary debug step.

## Broker module versioning

`mqtt_min_broker` versions are Git tags. Prefer a broker-specific prefix so
product bridge tags are not confused with broker module tags:

```bash
git tag minmqtt-v0.1.12
git push origin minmqtt-v0.1.12
```

Use a new tag when one of these changes:

- public broker API changes
- P2P behavior changes
- Zephyr module integration changes
- bug fix needed by a product app
- product-facing configuration behavior changes

The product app updates only after the module tag exists:

```json
"version": "minmqtt-v0.1.12"
```

## Downstream Status

Known downstream consumer:

| Product repo | Product tag | Broker tag | Status |
|--------------|-------------|------------|--------|
| `mqtt_field_bridge_app` | `bridge-v0.1.1` | `minmqtt-v0.1.12` | Synced and validated with Linux unit, integration, reconnect stress, and throughput stress tests |

## Build flow

Product build should run in this order:

1. Read `deps.json`.
2. Ensure `deps/mqtt_min_broker` exists.
3. Fetch tags for `mqtt_min_broker`.
4. Checkout the exact tag from `deps.json`.
5. Build the product app using that module path.

This keeps the product focused on a fixed, known broker version. Broker fixes
flow through the module repository and become available to the product only
after a new tag is selected.

## Completed Follow-Up

- Product dependency checks now validate that `deps.json` references existing
  module tags during product sync/test flows.
- Release notes are maintained per broker tag so product updates can review behavior
  changes before bumping.
