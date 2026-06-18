# P2P Shard Model

This document defines the first usable shard rule set for the broker mesh.

## Goal

Make the broker mesh scale by dividing topic ownership instead of syncing the
full subscription graph across all nodes.

The target workload is:

- 5 to 10 broker nodes
- many logical sensors
- many topics
- high publish rate

## Shard format

Use a topic prefix as the primary shard key:

```text
<site>/<zone>/<stream>/...
```

Examples:

- `factory/line1/temp`
- `factory/line1/humidity`
- `factory/line2/vibration`
- `buildingA/floor3/sensor42/state`

The first two or three topic levels define the shard boundary.

## Shard rule

For a topic `A/B/C/...`:

- `A` is the site
- `A/B` is the primary shard prefix
- `A/B/C` may be used as a sub-shard if `A/B` is too hot

The first implementation should use `A/B` as the shard owner key.

## Ownership

Each shard has:

- one primary owner broker
- zero or more neighbor brokers for routing

The owner broker is responsible for:

- accepting local publishes for that shard
- maintaining local subscriptions for that shard
- forwarding remote publishes to subscribers

Neighbor brokers store only shard summary data, not the full subscription set.

## Routing rules

1. If a publish topic belongs to the local shard, process it locally.
2. If it belongs to another shard, forward it to that shard owner.
3. If the local broker is not the owner but knows the owner, forward to owner.
4. If the owner is unreachable, forward to the nearest neighbor that has shard
   summary state.

## What not to do

- Do not broadcast every publish to every broker.
- Do not synchronize the full topic table across all nodes.
- Do not use a single ingress broker as the only publish path.

## Scaling principle

The mesh only gets faster if:

- publishers are distributed across brokers
- each broker owns a subset of shards
- cross-node state is incremental
- route lookup stays local most of the time

If all traffic still enters one broker or one shard, extra nodes only add
coordination cost.

## Next code step

Implement a small shard helper that:

- extracts the shard prefix from a topic
- maps shard prefix to owner
- exposes a route summary for gossip

After that, wire `topic_publish()` and `p2p_router_find_next_hops()` to consult
the shard helper before forwarding.
