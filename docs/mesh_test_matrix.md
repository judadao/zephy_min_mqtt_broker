# Mesh Test Matrix

This matrix turns the ESP32-mesh goal into a repeatable benchmark set.
It is designed for two comparisons:

1. `1 broker` vs `5 brokers` on the same workload shape.
2. `5 brokers` vs `10 brokers` under ESP32-like limits.

## Test Goals

- Verify that a 5- to 10-node ESP32 broker mesh can stay stable under heavy sensor traffic.
- Verify that the mesh can recover or degrade gracefully when one node fails.
- Verify that the mesh performs better than an isolated broker only when the topology and load split support it.

## Default Scenarios

The matrix runner is [`scripts/mesh_test_matrix.sh`](../scripts/mesh_test_matrix.sh).

| Scenario | Purpose | Expected signal |
|----------|---------|-----------------|
| `single_1` | Baseline single broker | Highest raw throughput, no routing overhead |
| `mesh_5` | 5-node ESP32-like mesh | Compare against `single_1` on the same topic/message shape |
| `mesh_5_node_down` | 5-node mesh with one broker killed | Measure availability under node loss |
| `mesh_5_restart` | 5-node mesh with one broker restarted | Measure recovery after restart |
| `mesh_5_dense` | 5-node mesh with denser seed fanout | Check whether denser topology improves loss/throughput |
| `mesh_10` | 10-node mesh stress | Check whether the design still scales when node count rises |

## Default Workload Shape

- topics: `100`
- messages: `1000`
- sensor clients: `0`
- distributed publishers: enabled for mesh runs
- message scaling by broker count: enabled for mesh runs
- target p95: `10 ms`

These defaults are intentionally conservative. They are meant to expose topology and routing problems first, not to maximize raw throughput.

## Pass / Fail Rules

- `received == messages`
- `p95 <= target_p95_ms`
- no benchmark process crash
- no broker container hang that requires manual cleanup

If `1 broker` wins on throughput but `5 brokers` loses messages or stalls, the mesh is not yet meeting the goal.

## Usage

```bash
./scripts/mesh_test_matrix.sh
```

The script writes a CSV to `build_out/mesh_test_matrix/results.csv` and leaves logs under `build_out/mesh_test_matrix/`.
