# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Current Progress

- Current direction: keep the original architecture of C++ master plus external
  nginx/WebDAV volume servers. Do not replace nginx with a C++ volume server.
- Recently completed work includes:
  - Upstream README parity review for the core API, nginx volume model,
    rebuild/rebalance workflow, and S3 compatibility subset.
  - Optional upstream-style Python S3 compatibility harness for real
    boto3/PyArrow clients, wired into CTest with dependency-aware skips.
  - S3 compatibility harness documents and reports optional boto3/PyArrow
    dependency status before running client tests.
  - `MINIKV_REQUIRE_S3_COMPAT_DEPS=1` can force the S3 compatibility harness
    to fail when boto3/PyArrow are missing instead of silently skipping them.
    Strict mode also enables the large multipart parquet roundtrip by default.
  - boto3/PyArrow are installed in the local `.venv` with `uv`, and strict
    `S3CompatTest` now passes against the five-volume deploy topology.
  - Upstream-style HTTP compatibility harness mirrors `tools/test.py` with
    real `requests` clients against the same five-volume deploy topology.
  - Production deploy templates now cover systemd master, systemd nginx volume
    instances, nginx/WebDAV volume config, and master environment config.
  - S3 `HEAD` returns direct object metadata for real S3 clients, and AWS SDK
    `aws-chunked` streaming payloads are decoded before replica writes.
  - S3 listings now include bucket/list metadata and object `LastModified` /
    `StorageClass` in addition to `Size` and `ETag`.
  - Go-style CLI parsing and `mkv server` executable entry point.
  - GET behavior closer to Go: fallback redirects, `Content-Md5`,
    `Key-Volumes`, `Key-Balance`, and random replica `HEAD` probing. HEAD now
    returns direct object metadata for S3 client compatibility.
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
  - S3-compatible `GET ?list-type=2` XML listing and `POST ?delete` bulk
    delete for bucket child keys.
  - S3 `ListObjectsV2` responses now include object `Size` and `ETag`
    metadata when a live replica can be probed.
  - S3-compatible multipart upload routes: `POST ?uploads`, `PUT
    ?partNumber=&uploadId=`, `POST ?uploadId=`, and `DELETE ?uploadId=`.
  - S3-style quoted MD5 `ETag` metadata for successful object `PUT`,
    multipart part upload, and multipart completion responses.
  - Multipart completion now rechecks overwrite protection, so an upload
    initialized before another writer creates the key cannot replace the live
    object.
  - Multipart scratch cleanup on startup and retryable completion when a part
    is missing.
  - Runtime multipart upload TTL cleanup removes abandoned part files and
    rejects expired upload ids; `-multipartttl` configures the window.
  - Multipart completion streams staged part files to replicas with
    `Content-Length` instead of concatenating the full completed object into one
    master-side string first.
  - HTTP adapter edge coverage for HEAD responses with non-zero
    `Content-Length`, percent-decoded paths/query strings, duplicate query
    keys, malformed percent escapes, stalled response timeouts, and custom
    methods.
  - Tests for CLI parsing, server read/write/delete flows, route wiring, and
    volume client behavior.
- Latest verified commands:
  - `UV_CACHE_DIR=/home/brice/minikv/.cache/uv /home/brice/.local/bin/uv pip
    install requests boto3 pyarrow`
  - `cmake --build --preset debug`
  - `MINIKV_REQUIRE_HTTP_COMPAT_DEPS=1 ctest --preset debug -R
    UpstreamCompatTest --output-on-failure`
  - `./build/debug/tests/mkv_tests --gtest_filter='HttpAdapterTest.AcceptsLargeRequestBodies:ServerRouteTest.GetAndHeadRoutesReturnRedirectLocation:ServerRouteTest.PutRouteDecodesAwsChunkedPayloads:ServerRouteTest.S3Multipart*:CliTest.*:ServerAppTest.StoresOptions'`
  - `./build/debug/tests/mkv_tests --gtest_filter='HashTest.*:VolumeClientTest.*:ServerRouteTest.S3Multipart*:ServerAppTest.StoresOptions'`
  - `MINIKV_REQUIRE_S3_COMPAT_DEPS=1 ctest --preset debug -R S3CompatTest
    --output-on-failure` passed with boto3, PyArrow, and the large multipart
    parquet roundtrip.
  - `ctest --preset debug` with `94/94 tests passed`
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
- Upstream-style HTTP compatibility CTest mirrors upstream `tools/test.py`
  against a five-volume, three-replica deploy topology using real `requests`
  clients.
- S3 compatibility CTest wrapper starts temporary nginx volumes and a C++
  master in a five-volume, three-replica deploy topology, then runs
  upstream-style boto3/PyArrow Python tests when those optional dependencies
  are installed.
- README includes production deploy instructions and `deploy/` includes
  systemd/nginx templates for master and volume services.
- README documents how to run `S3CompatTest`, why missing boto3/PyArrow suites
  are skipped, how to require those dependencies, and how to enable the larger
  multipart parquet roundtrip.
- S3-compatible XML listing and bulk delete smoke coverage against real
  nginx/WebDAV volumes.
- S3 list responses include per-object `Size` and `ETag` metadata from
  volume-server `HEAD`, plus bucket `Name`, `Prefix`, `KeyCount`,
  `LastModified`, and `StorageClass`, with unit and real boto3 coverage.
- S3-compatible multipart upload with per-DB namespaced temporary part files
  and nginx/WebDAV smoke coverage.
- S3 write responses include quoted MD5 `ETag` headers for normal writes,
  multipart part writes, and completed multipart objects; completed multipart
  XML also includes the final object ETag.
- S3-compatible abort multipart upload removes staged part files, invalidates
  the upload id, and leaves object metadata/remote replicas untouched.
- S3-compatible `HEAD` returns direct object metadata from volume replicas so
  PyArrow `get_file_info` sees real object size and ETag.
- AWS SDK `aws-chunked` streaming payloads are decoded before normal and
  multipart S3 PUT bodies are written to replicas.
- Multipart route tests cover completion-time overwrite rejection and lock
  conflicts for init, part upload, abort, and completion.
- Multipart startup cleanup removes stale scratch files left by previous
  processes, and failed completion due to missing parts keeps the upload id
  usable for retry.
- Runtime multipart TTL cleanup removes abandoned scratch parts during normal
  request handling; the TTL is configurable with `-multipartttl`.
- Multipart completion streams staged part files to volume replicas and
  computes the completed MD5 from those files, avoiding a full completed-object
  string in master memory before writes.
- HTTP adapter tests cover nginx-style HEAD responses with non-zero
  `Content-Length`, percent decoding for path/query parameters, duplicate
  query keys, malformed percent escapes, stalled response timeouts, and custom
  method forwarding.
- Master executable entry point with Go-style server flags.
- CMake presets use Ninja and 24-way parallel build jobs on this machine.

## Next

1. Broaden upstream parity checks.
   - Compare the local harness against upstream `tools/s3test.py` whenever
     upstream changes.
2. Add production benchmark evidence.
   - Run a repeatable deploy benchmark against the five-volume topology for
     PUT/GET/DELETE throughput, MiB/s, latency percentiles, and error counts.

## Remaining Capability Gaps

- S3 compatibility:
  - Current multipart support matches the upstream route shape and covers the
    real boto3/PyArrow workflows, but S3 error responses are still minimal
    compared with AWS XML error documents.
- Operational parity:
  - The C++ adapter now uses bounded server workers and timeout-aware async
    client operations, but it is not yet tuned like Go's `net/http` stack for
    high concurrency.

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
