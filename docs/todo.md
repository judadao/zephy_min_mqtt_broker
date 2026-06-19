# Broker Module TODO

Tracks open work items for the `mqtt_min_broker` module itself — no product-app or bridge-app items belong here.

## Zephyr Module

- [ ] Confirm `CONFIG_MQTT_MIN_BROKER=y` builds as a Zephyr module without `CONFIG_MQTT_STANDALONE`
- [ ] Add board-independent Zephyr build smoke test in CI or scripts
- [x] Verify `zephyr/module.yml`, `zephyr/Kconfig`, and `zephyr/CMakeLists.txt` are complete

## Public API

- [x] Document the minimal module API and lifecycle (what the embedder must call, in what order)
- [ ] Stable public API documentation (header-level, not a wiki)

## Release / Versioning

- [x] Add release notes file for module tags (`minmqtt-vX.Y.Z`)
- [x] Add first stable tag once module API boundaries are confirmed (`minmqtt-v0.1.0`)

## P2P Extension

- [ ] Static P2P seed API/config (for environments where UDP broadcast is unavailable)
- [ ] P2P peer status snapshot API (so embedders can query current peer connectivity)
- [ ] P2P route/remote subscription stats API

## Scope Guard

- [ ] Keep product-specific provisioning out of the broker core

## Test Gaps

- [ ] Verify SUBACK returns the correct granted-QoS byte (0x00/0x01/0x02) for each QoS level subscribed (§3.9.3)
- [ ] Verify retained message is cleared when a PUBLISH with zero-length payload is sent to the topic (§3.3.1.3)
- [ ] Verify client-ID takeover with clean_session=1 discards any QoS 2 inflight state from the previous persistent session
- [ ] Verify overlapping subscriptions deliver only one message copy to the subscribing client (§4.7.2)
