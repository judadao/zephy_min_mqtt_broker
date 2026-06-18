# Optimization Loop

This document tracks the working loop for shard-based P2P optimization.

## Goal

Improve throughput and reduce loss for the ESP32-like mesh workload where:

- broker count is small but non-trivial, especially `1` vs `5`
- topic count is around `100` to `200`
- sensor count is large, around `500` to `1000`
- message count is large, from `1000` to `50000`

The main target is to make broker capacity grow with node count instead of collapsing under coordination cost.

## Known Optimization Plan

1. Keep shard ownership stable.
   - Avoid owner churn from short-lived topology changes.
   - Prefer a stable owner unless the router set actually changed.

2. Reduce subscription resync cost.
   - Resync only when topology signature changes.
   - Batch resyncs instead of sending them one by one where possible.

3. Reduce relay fanout.
   - Forward only when there is a real downstream match.
   - Avoid router relays that do not improve reachability.

4. Reduce route lookup cost.
   - Keep exact and wildcard paths separate.
   - Avoid repeated full scans when a cache or index can answer the same question.

5. Reduce election pressure.
   - Election should not be on the hot path for message forwarding.
   - The router set should change slowly relative to publish traffic.

6. Keep benchmark scenarios stable.
   - Use the same workload shapes across iterations.
   - Change one variable at a time.

## Exploration Plan

Run experiments in this order:

1. Owner stability
   - immediate switch
   - topology epoch lock
   - cooling window

2. Resync trigger
   - role change
   - topology signature change
   - explicit owner change only

3. Shard key granularity
   - first path segment
   - first two path segments
   - first three path segments

4. Router density
   - `1`
   - `2`
   - `3`
   - `5`

5. Relay strategy
   - current conditional relay
   - no relay
   - exact-only relay
   - exact + wildcard split relay

6. Subscription resync mode
   - per-subscription
   - per-owner batch
   - one-shot after topology settles

## Scoring Rule

Keep the variant that performs best on the active benchmark set.

Primary score order:

1. lower `lost`
2. higher `msg/s`
3. lower `p95`

If two variants are close:

- prefer the simpler control path
- prefer fewer resyncs
- prefer fewer router-wide broadcasts

## Baseline Benchmarks

Use these as the default comparison set:

```bash
NETWORK_NAME=mqtt-p2p-bench
NETWORK_SUBNET=172.35.0.0/16
IP_PREFIX=172.35.1
SINGLE_BROKER_TOPICS=20
SINGLE_BROKER_MESSAGES=10
SINGLE_BROKER_SENSOR_CLIENTS=100
ESP32_BROKER_COUNTS="1 5"
ESP32_TOPICS="100"
ESP32_MESSAGES="1000"
ESP32_SENSOR_CLIENTS=0
./scripts/bench_esp32_workload.sh
```

For a heavier stress check, also run:

```bash
SENSOR_CLIENTS=500 SENSOR_CONNECTIONS=1 BROKER_COUNTS="1 5" \
TOPIC_COUNTS="200" MESSAGE_COUNTS="10000" STARTUP_SEC=2 SYNC_SETTLE_SEC=10 \
DISTRIBUTED_PUBLISHERS=1 SCALE_MESSAGES_BY_BROKER=1 \
./scripts/bench_p2p_docker_scale.sh
```

## Current Baseline

- `1 broker`: high throughput, higher latency
- `5 brokers`: lower latency, but throughput and loss are still not where they need to be

## Iteration Rule

1. Change one thing.
2. Run the same benchmark.
3. Record the numbers.
4. Keep only the best variant.
5. Repeat.

