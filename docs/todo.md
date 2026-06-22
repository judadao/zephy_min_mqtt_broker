# TODO

Source of truth: `docs/todo.yaml`. Update YAML before starting or completing work.

## repo

- [x] Add docs/todo.yaml so mqtt_min_broker is tracked globally.
- [x] Migrate the historical docs/todo.md checklist into structured YAML without losing release and test details.
- [x] Refactor broker module layout until it passes the module golden sample structure audit.

## validation

- [x] Reuse dephy_testkit broker and P2P fixtures in broker integration tests where they reduce duplication.
- [x] Guard unit_session create/find assertions against null dereference after allocation failure.
- [x] Clean up low-risk cppcheck findings in broker tests and CLI helpers.

## performance

- [x] Add a repeatable P2P routing benchmark for peer count, subscription count, and retained-message paths.
- [x] Audit fixed-size packet buffers and add explicit high-water or overflow regression coverage.
