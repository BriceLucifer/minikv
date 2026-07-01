# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Current Progress

- Current direction: keep the original architecture of C++ master plus external
  nginx/WebDAV volume servers. Do not replace nginx with a C++ volume server.
- Recently completed work includes:
  - Go-style CLI parsing and `mkv server` executable entry point.
  - GET/HEAD behavior closer to Go: fallback redirects, `Content-Md5`,
    `Key-Volumes`, `Key-Balance`, and random replica `HEAD` probing.
  - PUT route parity for empty bodies, overwrite rejection, and mutating-route
    key locking.
  - Query/list JSON responses for `?list` and `?unlinked`.
  - nginx/WebDAV end-to-end smoke coverage for `PUT`, redirecting `GET`,
    `HEAD`, `rebuild`, `rebalance`, and `DELETE`.
  - Command-line `rebuild` support for regenerating LevelDB metadata from
    existing nginx/WebDAV volume files.
  - Command-line `rebalance` support for copying objects to a new preferred
    volume set and deleting stale replicas.
  - Boost.Beast HTTP adapter so custom methods such as `UNLINK` and
    `REBALANCE` are routed by the C++ master.
  - Boost.JSON for query responses and nginx autoindex parsing.
  - HTTP server session handling now uses a bounded Boost.Asio thread pool
    instead of creating one thread per connection.
  - HTTP `UNLINK` route support for soft-deleting metadata without deleting
    remote volume files.
  - HTTP `REBALANCE` route support for moving one live key to its preferred
    target volumes.
  - Tests for CLI parsing, server read/write/delete flows, route wiring, and
    volume client behavior.
- Latest verified commands:
  - `cmake --build --preset debug`
  - `ctest --preset debug` with `69/69 tests passed`
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
- Boost.Beast HTTP adapter and remote volume client implementation.
- Boost.JSON dependency for HTTP JSON handling.
- Volume client tests using an in-process localhost HTTP server.
- Basic server app flows for write, read redirect, delete, and unlink.
- Thin `registerRoutes` HTTP wiring for `PUT`, `GET`, `HEAD`, `DELETE`,
  `UNLINK`, and `REBALANCE`.
- GET/HEAD fallback redirects, metadata headers, and randomized replica `HEAD`
  probing.
- PUT route empty-body rejection, overwrite rejection, and route-level key
  conflict handling.
- LevelDB prefix scanning plus JSON query responses for `?list`, `?unlinked`,
  `start`, and `limit`.
- Command-line `rebuild` that clears LevelDB, scans nginx/WebDAV autoindex
  JSON, decodes base64 object names, and reconstructs records.
- Command-line `rebalance` that verifies real replicas, copies object bytes to
  target volumes, updates LevelDB metadata, and deletes stale replicas.
- nginx/WebDAV end-to-end CTest smoke test, including rebuild recovery and
  rebalance migration.
- Master executable entry point with Go-style server flags.
- CMake presets use Ninja and 24-way parallel build jobs on this machine.

## Next

1. Implement upstream S3 compatibility routes.
   - `GET ?list-type=2` should return the upstream XML
     `ListBucketResult` response.
   - `POST ?delete` should parse S3 delete XML and bulk-delete child keys.
   - Multipart upload flow remains missing: `POST ?uploads`, `PUT
     ?partNumber=&uploadId=`, and `POST ?uploadId=`.
2. Tighten HTTP adapter behavior against real clients.
   - Add tests for HEAD responses with non-zero `Content-Length` and no body,
     matching nginx behavior.
   - Add tests for percent-encoded paths and query ordering edge cases.
3. Broaden upstream parity checks.
   - Compare C++ behavior against upstream `tools/s3test.py` after S3 routes
     exist.
   - Add route-level tests for `POST` conflict locking once POST/S3 support is
     implemented.

## Remaining Capability Gaps

- S3 compatibility:
  - `GET ?list-type=2` XML listing is not implemented.
  - `POST ?delete` bulk delete XML parsing is not implemented.
  - Multipart upload init/part/complete routes are not implemented.
- Exact upstream POST semantics:
  - Upstream uses `POST` for S3-only operations and locks
    `path + partNumber`; the C++ master currently rejects unknown methods with
    400 and has no POST route.
- Operational parity:
  - The C++ adapter is synchronous and intentionally small; it is functional
    for current tests but not yet tuned like Go's `net/http` server for high
    concurrency.

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
