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
- Basic server app flows for write, read redirect, delete, and unlink.
- Thin `registerRoutes` HTTP wiring for `PUT`, `GET`, `HEAD`, and `DELETE`.

## Next

1. Add query/JSON responses.
   - Match the useful parts of Go's `QueryHandler`.
   - Use nlohmann/json for response construction.

2. Improve GET/HEAD parity with the Go server.
   - Files: `include/server.hpp`, `src/server.cpp`, `tests/server_test.cpp`.
   - Add fallback redirects when configured.
   - Add `Content-Md5`, `Key-Volumes`, and `Key-Balance` response metadata.
   - Probe replicas with `remoteHead` before redirecting.

3. Add an executable entry point.
   - Parse db path, volumes, replica count, subvolume count, protect, md5sum, and listen address.
   - Construct `App`, register routes, and start the HTTP server.

4. Add end-to-end smoke test later.
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
