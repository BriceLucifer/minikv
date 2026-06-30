# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Current Progress

- Current direction: keep the original architecture of C++ master plus external
  nginx/WebDAV volume servers. Do not replace nginx with a C++ volume server.
- Recently completed work includes:
  - Go-style CLI parsing and `mkv server` executable entry point.
  - GET/HEAD behavior closer to Go: fallback redirects, `Content-Md5`,
    `Key-Volumes`, `Key-Balance`, and replica `HEAD` probing.
  - Tests for CLI parsing, server read/write/delete flows, route wiring, and
    volume client behavior.
- Latest verified commands:
  - `cmake --build --preset debug`
  - `ctest --preset debug` with `45/45 tests passed`
- Local environment note: `nginx` is not installed on this machine right now,
  so nginx-backed end-to-end testing needs either local nginx installation or a
  CI environment with nginx available.

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
- GET/HEAD fallback redirects, metadata headers, and replica `HEAD` probing.
- Master executable entry point with Go-style server flags.

## Next

1. Add an end-to-end smoke test.
   - Start one or more nginx/WebDAV volume servers.
   - Start the C++ master.
   - Use HTTP requests to verify `PUT`, redirecting `GET`, `HEAD`, and `DELETE`.

2. Improve GET/HEAD parity with the Go server.
   - Files: `include/server.hpp`, `src/server.cpp`, `tests/server_test.cpp`.
   - Match the random replica probing order from Go.
   - Decide whether to keep C++ route status `302` exactly or preserve any compatibility aliases.

3. Implement `rebuild` and `rebalance` commands.
   - The CLI accepts these commands for parity, but the executable currently reports them as not implemented.

4. Add query/JSON responses.
   - Match the useful parts of Go's `QueryHandler`.
   - Use nlohmann/json for response construction.

5. Revisit non-standard methods and advanced compatibility.
   - `UNLINK` is implemented in the business layer but not exposed through `cpp-httplib` routes because unknown HTTP methods are rejected before routing.
   - Delay S3 multipart, rebuild, and rebalance until the basic runnable system is stable.

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
