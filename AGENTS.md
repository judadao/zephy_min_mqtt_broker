# Repository Guidelines

## Project Structure & Module Organization

This repository contains a minimal MQTT v3.1.1 broker in C for Linux development and Zephyr/ESP32 deployment. Core broker logic lives in `src/` (`broker.c`, `client.c`, `packet.c`, `session.c`, `topic.c`), with public headers in `include/`. Platform-specific code is under `platform/posix/` and `platform/zephyr/`. The Linux CLI test client is `tools/mqtt_cli.c`. Zephyr module files are in `zephyr/`, with root `CMakeLists.txt`, `Kconfig`, and `prj.conf.template` supporting embedded builds. Generated outputs belong in `build_out/` and should not be committed.

## Build, Test, and Development Commands

- `make -f Makefile.linux`: builds the Linux broker and CLI into `build_out/mqtt_broker` and `build_out/mqtt_cli`.
- `make -f Makefile.linux DASHBOARD=1`: enables the optional HTTP dashboard on port `8080`.
- `make -f Makefile.linux AUTH_USER=admin AUTH_PASS=secret`: builds with compile-time username/password auth.
- `make -f Makefile.linux clean`: removes Linux build output.
- `./scripts/test_broker.sh`: builds if needed, starts the broker on port `1883`, and runs the smoke test suite.
- `west build -b esp32 .`: builds the Zephyr standalone app when a Zephyr environment is configured.

## Coding Style & Naming Conventions

Use C11-compatible C and keep code warning-clean under `-Wall -Wextra`. Follow the existing style: 4-space indentation, K&R-style braces for functions and control blocks, `snake_case` function names, and `UPPER_SNAKE_CASE` constants/macros. Keep module APIs in matching headers under `include/`, for example `src/topic.c` with `include/topic.h`. Avoid heap allocation in broker paths unless the design is explicitly changed.

## Testing Guidelines

The primary automated coverage is `scripts/test_broker.sh`, a Bash smoke suite using the local broker and `mqtt_cli`. Run it before submitting changes that affect MQTT packet handling, sessions, topics, retained messages, QoS, auth, or dashboard behavior. Add focused test cases to this script for new externally visible broker behavior. Ensure port `1883` is free before running tests.

## Commit & Pull Request Guidelines

Recent history uses short conventional prefixes such as `feat:`, `fix:`, `docs:`, and `refactor:`. Keep commit subjects imperative and specific, for example `fix: clear retained payload on empty publish`. Pull requests should describe the behavior change, list build/test commands run, mention platform impact, and link any related issue. Include screenshots only for dashboard UI changes.

## Security & Configuration Tips

Do not commit real WiFi or auth secrets. Copy `prj.conf.template` to local `prj.conf` for device credentials; `prj.conf` is intended to stay untracked. Treat generated firmware, maps, and binaries as build artifacts, not source.
