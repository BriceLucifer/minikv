# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Done

- CMake presets for debug/release builds.
- GoogleTest and CTest integration.
- Vendored dependencies through CMake `FetchContent`.
- BoringSSL-backed MD5 helper.
- Base64 encoding helper.
- Record encode/decode logic and tests.
- Placement logic for `key2path`, `key2volume`, and `needs_rebalance`.
- LevelDB-backed `LevelDbIndex` with put/get/delete tests.
- App state and key lock table scaffold.
- cpp-httplib dependency and remote volume client implementation.
- nlohmann/json dependency for upcoming HTTP JSON handling.
- Volume client tests using an in-process localhost HTTP server.

## Next

1. Implement basic server HTTP behavior.
   - Files: `include/server.hpp`, `src/server.cpp`, `tests/server_test.cpp`.
   - Start with `PUT`, `GET`, and `DELETE`.
   - Match Go's `WriteToReplicas`, `Get`, and `Delete` flow.
   - Use `volume_client` for remote volume operations.
   - Delay S3 multipart, rebuild, rebalance, list, and unlinked support until the core flow is stable.

2. Add query/JSON responses.
   - Match the useful parts of Go's `QueryHandler`.
   - Use nlohmann/json for response construction.

3. Add end-to-end smoke test later.
   - Start volume servers.
   - Start C++ master.
   - Use curl or pytest to verify PUT, GET, and DELETE.

## Useful Commands

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run one test:

```bash
./build/debug/tests/mkv_tests --gtest_filter=PlacementTest.*
```
