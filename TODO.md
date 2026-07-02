# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Current Progress

- Current direction: keep the original architecture of C++ master plus external
  nginx/WebDAV volume servers. Do not replace nginx with a C++ volume server.
- Latest upstream comparison point: `geohot/minikeyvalue` commit `451d248`
  (`make rebalance more robust (#40)`).
- Current parity summary:
  - Core upstream HTTP behavior is covered by `UpstreamCompatTest`, a real
    `requests` harness modeled after upstream `tools/test.py`.
  - Core S3 behavior is covered by `S3CompatTest`, a real boto3/PyArrow harness
    modeled after upstream `tools/s3test.py`.
  - Current verdict: the rewrite is functionally aligned for the upstream
    API/test surface, but it is not a byte-for-byte clone. The main intentional
    difference is `HEAD`: `GET` keeps the upstream 302 redirect model, while
    `HEAD` returns direct metadata so real S3/PyArrow clients work reliably.
  - Latest verification: strict HTTP/S3 compatibility tests pass against a
    temporary five-volume nginx/WebDAV deploy, and the full debug test suite
    passes with `101/101` tests.
  - The C++ rewrite is now ahead of upstream for S3 metadata, AWS chunked
    upload compatibility, runtime multipart cleanup, streaming multipart
    completion, deploy templates, and bounded worker execution.
  - Local release benchmark evidence is now recorded in README for both the
    C++ rewrite and upstream Go on the same MacBook/nginx topology. Remaining
    production-complete gaps are staging benchmark evidence and live
    monitoring/alert validation.
  - Production hardening now includes a configurable HTTP body limit, pinned
    fetched dependency revisions, defensive placement validation, S3-style XML
    error bodies for common failures, and concrete backup/restore/monitoring
    runbook guidance.
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
  - HTTP/1.1 keep-alive, master-to-volume keep-alive pooling, configurable
    `-volumepool`, and `tools/thrasher.go` benchmark parity tooling.
  - README and `experience.txt` were compacted into deploy/runbook and
    production-performance notes; the local benchmark machine is recorded as
    MacBook Pro Mac16,8, Apple M4 Pro 14-core, 24 GB RAM, macOS 26.5.1.
  - Full C++ clang-tidy coverage was run across every `src/*.cpp` and
    `tests/*.cpp`; clangd unused-include findings in `src/server.cpp` and
    `tests/volume_client_test.cpp` were fixed.
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
  - `cmake --build --preset debug`
  - `ctest --preset debug --output-on-failure` with `101/101 tests passed`
  - `cmake --build --preset release`
  - `clang-tidy -p build/debug $(rg --files -g '*.cpp' src tests) --quiet`
    completed without warnings
  - `clangd --check` was run across all C++ translation units; this local
    clangd reports non-source `ExtractFunction` tweak errors on test macros,
    while clang-tidy and compilation remain clean
  - `go run tools/thrasher.go -h` verified the checked-in thrasher tool
  - Short post-cleanup benchmark probe with
    `/private/tmp/minikv_benchmark.py --reuse-client-connections` completed
    every scenario with `300/300` operations and `0` errors; c16 64 KiB
    `GET -L` measured about `5.23k req/s` and c16 64 KiB PUT+DELETE measured
    about `1.73k ops/s`
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
- CMake presets use Ninja and leave build parallelism to Ninja's platform
  default so each machine can use its own detected worker count.

## Next

1. Improve S3 error compatibility.
   - Done for common write/multipart failures: `AccessDenied`, `MalformedXML`,
     `InvalidArgument`, and `NoSuchUpload`.
   - Remaining: broaden XML error coverage for read/list misses such as
     `NoSuchKey` and bucket-shaped path misses where clients depend on those
     exact codes.
2. Promote benchmark evidence from local to staging.
   - Local MacBook benchmark evidence is recorded in README. Repeat it on a
     production-like Linux host with persistent disks, fixed process limits,
     `wrk`, `tools/thrasher.go`, fd/TIME_WAIT capture, and perf/flamegraphs.
   - Record release-build hardware, object size mix, replica count, worker
     count, volume pool width, and disk path in README/TODO so results remain
     comparable later.
3. Validate AI-agent memory operations runbooks in staging.
   - README now documents key naming, retention, lifecycle cleanup,
     backup/restore drills, rebuild/rebalance procedures, and recommended
     external metadata/vector DB integration.
   - Remaining: run the documented restore and rebalance drills against a real
     staging topology and record operator timings/failure modes.
4. Keep upstream parity checks current.
   - Compare local HTTP/S3 harnesses against upstream `tools/test.py` and
     `tools/s3test.py` whenever upstream changes.

## Remaining Capability Gaps

- S3 compatibility:
  - Current multipart support matches the upstream route shape and covers the
    real boto3/PyArrow workflows. Common write/multipart failures now return
    S3-style XML errors, but read/list miss coverage is still not complete AWS
    compatibility.
- Operational parity:
  - The C++ adapter now uses bounded server workers, timeout-aware async client
    operations, and a configurable request body limit, but it is not yet tuned
    like Go's `net/http` stack for high concurrency.
- AI agent memory foundation:
  - The README describes using minikv for raw artifacts alongside a metadata DB
    and vector DB and now includes lifecycle, retention, backup/restore, and
    monitoring runbook guidance. Staging validation remains to be recorded.

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
