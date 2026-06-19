# Broker Module TODO

Tracks open work items for the `mqtt_min_broker` module itself — no product-app or bridge-app items belong here.

## Zephyr Module

- [ ] Confirm `CONFIG_MQTT_MIN_BROKER=y` builds as a Zephyr module without `CONFIG_MQTT_STANDALONE`
- [x] Add board-independent Zephyr build smoke test in CI or scripts
- [x] Verify `zephyr/module.yml`, `zephyr/Kconfig`, and `zephyr/CMakeLists.txt` are complete

## Public API

- [x] Document the minimal module API and lifecycle (what the embedder must call, in what order)
- [x] Stable public API documentation (header-level, not a wiki)

## Release / Versioning

- [x] Add release notes file for module tags (`minmqtt-vX.Y.Z`)
- [x] Add first stable tag once module API boundaries are confirmed (`minmqtt-v0.1.0`)

## P2P Extension

- [x] Static P2P seed API/config (for environments where UDP broadcast is unavailable)
- [x] P2P peer status snapshot API (so embedders can query current peer connectivity)
- [x] P2P route/remote subscription stats API

## Scope Guard

- [ ] Keep product-specific provisioning out of the broker core

## Next Iteration Plan

- [ ] Add header-level public API docs for broker/session/topic lifecycle and embedder responsibilities
- [x] Add a Zephyr module smoke script or CI job for `CONFIG_MQTT_MIN_BROKER=y`
- [ ] Add P2P peer/subscription snapshot APIs for dashboard or embedder observability
- [x] Add malformed-packet integration coverage for SUBSCRIBE requested-QoS parser edge cases
- [ ] Review remaining fixed-size buffers for explicit overflow test coverage

## Test Gaps

- [x] Verify SUBACK returns the correct granted-QoS byte (0x00/0x01/0x02) for each QoS level subscribed (§3.9.3)
- [x] Verify retained message is cleared when a PUBLISH with zero-length payload is sent to the topic (§3.3.1.3)
- [x] Verify client-ID takeover with clean_session=1 discards any QoS 2 inflight state from the previous persistent session
- [x] Verify overlapping subscriptions deliver only one message copy to the subscribing client (§4.7.2)
- [x] Reject SUBSCRIBE requested-QoS bytes with reserved bits set or QoS=3 (§3.8.3)
