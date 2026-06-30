# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Current Progress

- Current direction: keep the original architecture of C++ master plus external
  nginx/WebDAV volume servers. Do not replace nginx with a C++ volume server.
- Recently completed work includes:
  - Go-style CLI parsing and `mkv server` executable entry point.
  - GET/HEAD behavior closer to Go: fallback redirects, `Content-Md5`,
    `Key-Volumes`, `Key-Balance`, and replica `HEAD` probing.
  - PUT route parity for empty bodies, overwrite rejection, and mutating-route
    key locking.
  - Query/list JSON responses for `?list` and `?unlinked`.
  - nginx/WebDAV end-to-end smoke coverage for `PUT`, redirecting `GET`,
    `HEAD`, `rebuild`, and `DELETE`.
  - Command-line `rebuild` support for regenerating LevelDB metadata from
    existing nginx/WebDAV volume files.
  - Tests for CLI parsing, server read/write/delete flows, route wiring, and
    volume client behavior.
- Latest verified commands:
  - `cmake --build --preset debug`
  - `ctest --preset debug --output-on-failure` with `62/62 tests passed`
- Local environment note: `nginx` is installed on this machine and the CTest
  suite now includes `NginxSmokeTest`.

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
- PUT route empty-body rejection, overwrite rejection, and route-level key
  conflict handling.
- LevelDB prefix scanning plus JSON query responses for `?list`, `?unlinked`,
  `start`, and `limit`.
- Command-line `rebuild` that clears LevelDB, scans nginx/WebDAV autoindex
  JSON, decodes base64 object names, and reconstructs records.
- nginx/WebDAV end-to-end CTest smoke test, including rebuild recovery.
- Master executable entry point with Go-style server flags.

## Next

1. Implement `rebalance`.
   - Iterate records from LevelDB.
   - Verify existing replicas with `HEAD`.
   - Copy data to target volumes, update metadata, and delete stale replicas.
   - Add focused unit tests plus an nginx-backed smoke test.

2. Improve GET/HEAD parity with the Go server.
   - Files: `include/server.hpp`, `src/server.cpp`, `tests/server_test.cpp`.
   - Match the random replica probing order from Go.
   - Decide whether to keep C++ route status `302` exactly or preserve any compatibility aliases.

3. Revisit non-standard methods and advanced compatibility.
   - `UNLINK` is implemented in the business layer but not exposed through `cpp-httplib` routes because unknown HTTP methods are rejected before routing.
   - Add HTTP `REBALANCE` method support after command-line rebalance is stable.
   - Delay S3 multipart until rebuild/rebalance are stable.

## Remaining Capability Gaps

- Migration: command-line `rebalance` is still missing, so changing the volume
  set cannot yet copy objects to their new preferred replicas.
- Read distribution: GET/HEAD replica probing works, but still probes in stored
  order instead of the Go server's randomized order.
- Compatibility methods: business logic supports virtual unlink, but HTTP
  `UNLINK` is not exposed through the router; HTTP `REBALANCE` is also not
  implemented yet.
- S3 compatibility: S3-style bucket listing, bulk delete, and multipart upload
  flows remain deferred until rebuild/rebalance are stable.

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
