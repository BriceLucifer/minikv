# minikv

C++23 rewrite scaffold for `geohot/minikeyvalue`.

`minikeyvalue` is a small distributed object/blob store. The master keeps key
metadata in LevelDB, while volume servers store the actual bytes on disk behind
plain HTTP. Clients use simple `PUT`, `GET`, and `DELETE` requests; the master
chooses replica volumes, records the placement, and redirects reads to a volume.

For agent systems, this can act as artifact storage: store large inputs,
outputs, tool logs, browser screenshots, traces, generated files, and cached
blobs by key. Keep structured metadata in SQLite/Postgres and embeddings in a
vector database; use minikv for the raw artifact bytes.

Example agent artifact keys:

```text
/runs/abc/input.json
/runs/abc/output.json
/runs/abc/tool_calls.jsonl
/runs/abc/screenshots/0001.png
/runs/abc/artifacts/result.zip
```

For an AI agent memory stack, use this service as the durable blob/artifact
layer rather than the semantic index itself:

```text
Postgres/SQLite  run metadata, ownership, retention, lineage
Vector DB        embeddings and semantic retrieval
minikv           raw bytes: traces, screenshots, prompts, outputs, datasets
```

The key contract should be deterministic and append-friendly. Examples:

```text
/agents/{agent_id}/runs/{run_id}/trace.jsonl
/agents/{agent_id}/runs/{run_id}/screenshots/0001.png
/agents/{agent_id}/memory/{memory_id}/source.bin
/datasets/{dataset_id}/shards/{shard_id}.parquet
```

Example API usage:

```bash
curl -X PUT --data-binary @trace.jsonl \
  http://localhost:3000/runs/abc/trace.jsonl

curl -L http://localhost:3000/runs/abc/trace.jsonl

curl -X DELETE http://localhost:3000/runs/abc/trace.jsonl
```

The project uses CMake presets and fetches third-party dependencies during
configure. You do not need to install GoogleTest, LevelDB, BoringSSL, or Boost
with `apt`.

## Mental Model

The shortest way to understand minikeyvalue is:

```text
master stores metadata; volume servers store bytes.
```

It is not a plain in-process `key -> value` map. It is closer to a small object
store:

```text
key -> LevelDB metadata -> real file on a volume server
```

For example, a client writes:

```text
PUT /hello
body = "payload"
```

The master does not store `"payload"` in LevelDB. Instead, it:

1. Chooses replica volumes with `key2volume`.
2. Writes a `SOFT` record to LevelDB so readers do not see a partial write.
3. Sends the bytes to each selected nginx/WebDAV volume server.
4. Writes a final `NO` record after all replicas succeed.

The LevelDB record says where the object lives:

```text
/hello -> volume-a,volume-b
```

The actual bytes live as files on the volume servers.

Reads work the other way around:

1. The client asks the master for `GET /hello`.
2. The master reads the LevelDB record.
3. The master probes replicas with `HEAD`.
4. The master returns a `302 Location: http://volume/...` redirect.
5. The client downloads bytes directly from the volume server.

This avoids sending large downloads through the master. The master stays a
metadata and coordination service; nginx serves the large file bodies.

Deletes are also coordinated through metadata:

1. The master reads the current record.
2. It writes a `SOFT` record to mark the key as being deleted.
3. It asks each volume server to delete the file.
4. It removes the LevelDB key after remote deletes succeed.

Deletion states:

```text
NO    object exists and can be read
SOFT  write/delete is in progress; readers should treat it as unavailable
HARD  no LevelDB record exists for this key
```

The important helper functions map onto this model:

```text
record      encode/decode LevelDB metadata
placement   choose replica volumes and object file paths
index       read/write LevelDB records
volume_client  talk to nginx/WebDAV volume servers
server      coordinate PUT, GET/HEAD redirect, and DELETE
cli/main    start the C++ master process
```

## Dependencies

Required locally:

- CMake 3.20+
- A C++23 compiler
- Git

Fetched by CMake:

- GoogleTest
- LevelDB
- BoringSSL
- Boost.Asio
- Boost.Beast
- Boost.JSON

Runtime services:

- `nginx` with the WebDAV module for volume servers
- One writable filesystem directory per volume
- One LevelDB directory for the master index

Optional compatibility test clients are managed by `uv` from
`pyproject.toml`:

- `requests`
- `boto3`
- `pyarrow`

## Build And Test

Configure debug build:

```bash
cmake --preset debug
```

Build debug:

```bash
cmake --build --preset debug
```

The debug build produces:

```text
build/debug/mkv
```

Run tests:

```bash
ctest --preset debug
```

Run the test binary directly:

```bash
./build/debug/tests/mkv_tests
```

Run the optional upstream-style S3 client compatibility harness:

```bash
ctest --preset debug -R S3CompatTest --output-on-failure
```

`S3CompatTest` starts temporary nginx/WebDAV volume storage and a C++ master,
then runs `tests/s3_compat_test.py`. The topology follows the upstream deploy
shape with five nginx/WebDAV volumes behind one master using three replicas.
The Python suite mirrors the intent of upstream `tools/s3test.py`: boto3
write/list/delete, the known redirect-based `get_object` expected failure, and
PyArrow file-info/list/delete/parquet checks. The shell harness calls
`tests/ensure_python_test_env.sh`, which runs `uv sync` from `pyproject.toml`
and uses the repository-local `.venv`. Set `MINIKV_RUN_LARGE_S3_COMPAT=1` to
include the larger multipart parquet roundtrip.

For a strict environment where missing boto3/PyArrow should fail the test
instead of skipping it:

```bash
MINIKV_REQUIRE_S3_COMPAT_DEPS=1 \
  ctest --preset debug -R S3CompatTest --output-on-failure
```

Strict dependency mode also enables the large multipart parquet roundtrip by
default, matching the upstream `tools/s3test.py` coverage more closely.

Check dependency availability without starting the local topology:

```bash
python3 tests/s3_compat_test.py --check-deps
```

Run the upstream-style HTTP client compatibility harness:

```bash
MINIKV_REQUIRE_HTTP_COMPAT_DEPS=1 \
  ctest --preset debug -R UpstreamCompatTest --output-on-failure
```

`UpstreamCompatTest` mirrors upstream `tools/test.py` against a temporary
deploy topology with five nginx/WebDAV volumes and one master. It covers the
original HTTP API: writes, redirects, deletes, range requests, HEAD metadata,
large objects, JSON listing, empty-body rejection, and `Content-Md5`.

### Test Artifacts And Cleanup

Most C++ tests are self-contained, but the deploy-style shell tests create
real localhost services and temporary filesystem roots:

- `NginxSmokeTest` creates `${TMPDIR:-/tmp}/minikv-nginx-smoke.*`, starts
  nginx volume servers, then starts a local `mkv server`.
- `S3CompatTest` creates `${TMPDIR:-/tmp}/minikv-s3-compat.*`, starts five
  nginx/WebDAV volume servers, then starts one local master.
- `UpstreamCompatTest` creates `${TMPDIR:-/tmp}/minikv-upstream-compat.*`,
  starts the same five-volume topology, then runs the Python upstream client
  checks.

These scripts install EXIT traps that kill their nginx/master processes and
remove their temporary roots. If a test is interrupted very early, killed with
`SIGKILL`, or fails before the trap is installed, those directories can be left
behind. On macOS they usually appear under the per-user `$TMPDIR`, for example
`/private/var/folders/.../T/`, not necessarily under `/tmp`.

Check for leftovers with:

```bash
find "${TMPDIR:-/tmp}" /tmp -maxdepth 1 \
  \( -name 'minikv-nginx-smoke.*' \
     -o -name 'minikv-s3-compat.*' \
     -o -name 'minikv-upstream-compat.*' \
     -o -name 'minikv-index-test' \
     -o -name 'minikv-rebuild-test' \
     -o -name 'minikv-rebalance-test' \
     -o -name 'minikv-server-test' \
     -o -name 'minikv-remote-put-files-test' \) \
  -print
```

Remove those temporary roots with the same `find` command plus `-exec rm -rf
{} +` after reviewing the printed paths. The repository-local `.venv` and
`.cache/uv` directories are Python dependency caches created by
`tests/ensure_python_test_env.sh`; keep them unless you intentionally want to
force a fresh dependency sync.

## Upstream Parity Status

This rewrite is functionally aligned with upstream `geohot/minikeyvalue` at
commit `451d248` for the main operational surface. It is API-compatible for
the upstream-tested behavior, but it is not intended to be a byte-for-byte
clone where a stronger client-facing behavior is needed:

- Original HTTP API: `GET`, `PUT`, `DELETE`, `UNLINK`, `REBALANCE`, `?list`,
  `?unlinked`, range reads through nginx redirects, duplicate write/delete
  behavior, empty-body rejection, and `Content-Md5`.
- Storage topology: one master with LevelDB metadata, external nginx/WebDAV
  volume servers, simple filesystem blob layout, rebuild from volume files,
  and rebalance across changed preferred volume sets.
- CLI shape: `server`, `rebuild`, and `rebalance` commands with Go-style flags
  for database path, volumes, replicas, subvolumes, fallback, protect mode,
  MD5, and volume timeout.
- S3 subset: list, bulk delete, multipart upload, boto3 write/list/delete, the
  upstream redirect limitation for boto3 `get_object`, and PyArrow
  file-info/list/delete/parquet workflows.

Compatibility is verified by real deploy-style tests, not only unit tests:

- `UpstreamCompatTest` mirrors upstream `tools/test.py` with a temporary
  five-volume nginx/WebDAV deployment and a C++ master.
- `S3CompatTest` mirrors upstream `tools/s3test.py` with real boto3/PyArrow
  clients; strict mode requires those dependencies and runs the large parquet
  multipart roundtrip.
- The current debug preset passes `ctest --preset debug` with all 101 tests.

The C++ rewrite is intentionally stronger than upstream in several places:

- Bounded worker pool instead of one thread per accepted connection.
- HTTP/1.1 keep-alive on inbound master requests with bounded requests per
  connection and idle timeouts.
- Timeout-aware remote volume client operations.
- Ordinary master-to-volume `GET`/`HEAD`/`PUT`/`DELETE` calls reuse a
  conservative per-volume keep-alive connection.
- S3 `HEAD` returns direct object metadata while `GET` keeps upstream's 302
  redirect model.
- S3 list entries include `Size`, `ETag`, `LastModified`, and `StorageClass`.
- AWS SDK `aws-chunked` request bodies are decoded before replica writes.
- HTTP request bodies are bounded by `-maxbodysize` so one oversized upload
  cannot grow master memory without a configured limit.
- Multipart scratch space is per database path, cleaned at startup, can expire
  while the process is running, and multipart completion streams part files to
  replicas instead of first building one full object string.
- Common S3 failures return XML error bodies with S3-style error codes such as
  `AccessDenied`, `MalformedXML`, `InvalidArgument`, and `NoSuchUpload`.
- Production templates are provided for systemd master and nginx volume
  services.
- CMake pins fetched LevelDB and BoringSSL revisions for reproducible release
  builds.
- `tools/thrasher.go` provides a local clone of upstream's write/read/delete
  thrasher with configurable master URL.

Known remaining gaps before calling the rewrite production-complete:

- Multipart file streaming and simple standalone clients still use one-shot
  HTTP connections; use a keep-alive capable benchmark client when measuring
  the server's real CPU or disk limit.
- The master-to-volume keep-alive pool is configurable with `-volumepool`.
  Raising it can increase fd pressure and was slower on the local macOS
  benchmark after nginx/WebDAV became the wait point.
- The master still materializes accepted `PUT` request bodies in memory up to
  `-maxbodysize`; size that limit for your object mix and worker count.
- Authentication and TLS are expected to sit in front of the service; they are
  not built into the master.

## Run With nginx Volumes

This rewrite keeps the original minikeyvalue storage model: the master is C++,
while volume servers are external nginx/WebDAV file servers.

Start volume servers using the original `volume` script or equivalent nginx
configuration:

```bash
PORT=3001 ./volume /tmp/volume1/ &
PORT=3002 ./volume /tmp/volume2/ &
PORT=3003 ./volume /tmp/volume3/ &
```

Start the C++ master:

```bash
./build/debug/mkv \
  -volumes localhost:3001,localhost:3002,localhost:3003 \
  -db /tmp/indexdb \
  -multipartttl 24h \
  -maxbodysize 1G \
  server
```

Then use the original API shape:

```bash
curl -v -L -X PUT -d hello localhost:3000/hello
curl -v -L localhost:3000/hello
curl -v -L -X DELETE localhost:3000/hello
```

The C++ master also implements the upstream S3 compatibility subset for bucket
listing, bulk delete, metadata reads, and multipart upload:

```bash
curl -s 'localhost:3000/bucket?list-type=2'
curl -X POST -H 'Content-Type: application/xml' \
  --data-binary '<Delete><Object><Key>file.txt</Key></Object></Delete>' \
  'localhost:3000/bucket?delete'
curl -I 'localhost:3000/bucket/file.txt'

UPLOAD_ID=$(curl -s -X POST 'localhost:3000/bucket/large.bin?uploads' |
  sed -n 's:.*<UploadId>\([^<]*\)</UploadId>.*:\1:p')
curl -X PUT --data-binary @part-1.bin \
  "localhost:3000/bucket/large.bin?partNumber=1&uploadId=$UPLOAD_ID"
curl -X PUT --data-binary @part-2.bin \
  "localhost:3000/bucket/large.bin?partNumber=2&uploadId=$UPLOAD_ID"
curl -X POST -H 'Content-Type: application/xml' \
  --data-binary '<CompleteMultipartUpload><Part><PartNumber>1</PartNumber></Part><Part><PartNumber>2</PartNumber></Part></CompleteMultipartUpload>' \
  "localhost:3000/bucket/large.bin?uploadId=$UPLOAD_ID"
```

Multipart part files are stored under a path derived from the LevelDB path
instead of global `/tmp`, so multiple deployments do not share upload scratch
space by accident. Stale multipart scratch files from a previous process are
removed on startup, and a completion request that references a missing part
keeps the upload id valid so the client can upload the missing part and retry.
Abandoned uploads are also expired while the process is running; configure the
runtime window with `-multipartttl`, using plain milliseconds or `ms`, `s`,
`m`, and `h` suffixes.

Multipart completion streams staged part files to each replica with a known
`Content-Length` and computes the completed object MD5 from the part files.
This keeps completion aligned with upstream's `io.MultiReader` model and avoids
building the full completed object in one master-side string before replica
writes.

S3-style listings include `Name`, `Prefix`, `KeyCount`, object `Size`, `ETag`,
`LastModified`, and `StorageClass`. `HEAD` returns direct object metadata for
S3 clients while `GET` keeps the original minikeyvalue redirect model.

## Production Deploy

The production shape should stay close to upstream:

```text
clients -> mkv master -> nginx/WebDAV volume servers -> local filesystems
```

The master stores only LevelDB metadata. Volume servers store object bytes on
ordinary filesystems behind stock nginx/WebDAV. Do not point multiple masters
at the same LevelDB path; stop the master before running `rebuild` or
`rebalance`.

Example filesystem layout:

```text
/usr/local/bin/mkv
/etc/minikv/master.env
/etc/minikv/volume-1.conf
/var/lib/minikv/indexdb/
/var/lib/minikv/volume-1/
/var/lib/minikv/volume-2/
/var/lib/minikv/volume-3/
```

Example service/config templates are in `deploy/`:

```text
deploy/minikv-master.env.example
deploy/minikv-master.service
deploy/minikv-volume@.service
deploy/nginx-volume.conf.example
```

### 1. Build A Release Binary

Build and test the same commit you plan to deploy:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset debug --output-on-failure
```

Install the binary:

```bash
sudo install -m 0755 build/release/mkv /usr/local/bin/mkv
```

Keep the deployed git commit, compiler, CMake preset, and dependency revisions
with your release notes. LevelDB and BoringSSL are pinned by CMake.

### 2. Deploy nginx Volume Services

Each volume is an nginx/WebDAV server that owns one filesystem root. One host
can run several volume services by copying
`deploy/nginx-volume.conf.example` to `/etc/minikv/volume-1.conf`,
`/etc/minikv/volume-2.conf`, and so on, then changing `listen`, `pid`, log, and
`root` paths. Start them with systemd instance names:

```bash
sudo install -d /etc/minikv /var/lib/minikv
sudo cp deploy/minikv-volume@.service /etc/systemd/system/

sudo cp deploy/nginx-volume.conf.example /etc/minikv/volume-1.conf
sudo cp deploy/nginx-volume.conf.example /etc/minikv/volume-2.conf
sudo cp deploy/nginx-volume.conf.example /etc/minikv/volume-3.conf

# Edit each copied file:
# - listen port, for example 3001, 3002, 3003
# - pid path
# - error_log path
# - root path
# - client_body_temp_path

sudo systemctl daemon-reload
sudo systemctl enable --now minikv-volume@1
sudo systemctl enable --now minikv-volume@2
sudo systemctl enable --now minikv-volume@3
```

Volume ports should normally be private to the master and trusted clients. If
volumes are reachable from untrusted networks, put authentication/TLS/firewall
rules in front of them.

Verify each volume before starting the master:

```bash
curl -i http://127.0.0.1:3001/
curl -X PUT --data-binary test http://127.0.0.1:3001/smoke/object
curl -i http://127.0.0.1:3001/smoke/object
curl -X DELETE http://127.0.0.1:3001/smoke/object
```

### 3. Configure And Start The Master

Install the master unit and env file:

```bash
sudo cp deploy/minikv-master.env.example /etc/minikv/master.env
sudo cp deploy/minikv-master.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Edit `/etc/minikv/master.env` before starting production. `MINIKV_VOLUMES`
must list every volume address the master should use, and
`MINIKV_REPLICAS` must be no larger than that list. Keep `MINIKV_SUBVOLUMES`
stable for an existing deployment unless you are deliberately rebalancing.

Example single-host test deployment:

```text
MINIKV_PORT=3000
MINIKV_DB=/var/lib/minikv/indexdb
MINIKV_VOLUMES=127.0.0.1:3001,127.0.0.1:3002,127.0.0.1:3003
MINIKV_REPLICAS=3
MINIKV_SUBVOLUMES=10
MINIKV_VOLTIMEOUT=1s
MINIKV_MULTIPART_TTL=24h
MINIKV_MAX_BODY_SIZE=1G
MINIKV_WORKERS=0
MINIKV_VOLUME_POOL=8
MINIKV_PARALLEL_REPLICAS=false
MINIKV_EXTRA_ARGS=
```

Important production settings:

- `MINIKV_MAX_BODY_SIZE`: largest accepted single request body. The master can
  hold roughly `active writes * max body size` before replica writes complete.
- `MINIKV_WORKERS`: HTTP worker threads. `0` uses a keep-alive aware automatic
  default of at least 64. Because the current server handles each persistent
  connection synchronously, size this above expected concurrent client
  connections and watch p99 latency, RSS, fd count, and TIME_WAIT.
- `MINIKV_VOLUME_POOL`: keep-alive connections per volume endpoint for ordinary
  `GET`/`HEAD`/`PUT`/`DELETE` replica operations. The local benchmark default
  is `8`; increasing it to `16` on macOS increased fd pressure and reduced
  thrasher throughput.
- `MINIKV_PARALLEL_REPLICAS`: keep `false` for production by default. `true`
  writes/deletes replicas concurrently, but it also increases simultaneous
  volume pressure and should be enabled only with connection metrics.
- `MINIKV_VOLTIMEOUT`: volume `HEAD` timeout used on read redirects. Keep it
  low enough that a failed volume does not hold master workers for long.

Start the master:

```bash
sudo systemctl enable --now minikv-master
sudo systemctl status minikv-master
```

Run a master smoke test:

```bash
curl -i -X PUT --data-binary hello http://127.0.0.1:3000/smoke/hello
curl -i http://127.0.0.1:3000/smoke/hello
curl -L http://127.0.0.1:3000/smoke/hello
curl -i -X DELETE http://127.0.0.1:3000/smoke/hello
```

Expected behavior:

- `PUT` returns `201`.
- `GET` without `-L` returns `302 Location: http://volume/...`.
- `GET -L` returns the object bytes.
- `DELETE` returns `204`.

### 4. Preflight Gates

Before accepting traffic, run the same gates used by this repo:

```bash
cmake --build --preset release
ctest --preset debug
MINIKV_REQUIRE_HTTP_COMPAT_DEPS=1 \
  ctest --preset debug -R UpstreamCompatTest --output-on-failure
MINIKV_REQUIRE_S3_COMPAT_DEPS=1 \
  ctest --preset debug -R S3CompatTest --output-on-failure
```

For a machine without system Python packages, let `uv` create and sync the
repository-local test environment:

```bash
tests/ensure_python_test_env.sh
```

Do not skip the deploy-style tests for a production release. They start real
nginx/WebDAV volumes and catch failures that unit tests cannot see.

### 5. Production Runbook

- Backup: take filesystem snapshots of `/var/lib/minikv/indexdb` and every
  volume root together when possible. If snapshots cannot be atomic, snapshot
  volume roots first, then LevelDB, and run a restore drill before trusting the
  backup policy.
- Restore drill: restore volumes to a staging host, start nginx volume services,
  run `mkv -volumes ... -db /tmp/rebuilt-index rebuild`, then run HTTP and S3
  compatibility tests against that staging topology.
- Rebuild: stop the master before rebuilding a production index. Rebuild into a
  new DB path first, then swap the service env to the rebuilt path after
  validation.
- Rebalance: stop the master, update `MINIKV_VOLUMES`, run `rebalance`, then
  restart the master. Keep old volumes online until `Key-Balance` reports
  balanced for sampled hot keys and the rebalance command exits cleanly.
- Delete workflow: use `-protect` with `UNLINK` plus `?unlinked` when operators
  need a review window before physically deleting volume files.
- Monitoring: alert on 5xx responses, 409 conflicts, 413 payload rejections,
  volume `HEAD`/`PUT`/`DELETE` failures, LevelDB open/write failures, nginx
  disk usage, inode usage, master RSS, and growth of the `*.multipart`
  scratch directory.
- Capacity: keep free space above one full replica set of your largest expected
  object batch. For agent artifact storage, define retention per key prefix and
  run periodic `?list` scans from your metadata DB ownership model.
- Keep volume ports private to trusted clients or put authentication/TLS in
  front of the master and volumes.

### 6. Benchmark And Readiness Gate

Benchmark gate:

```bash
cmake --preset release
cmake --build --preset release

# Start the same five-volume nginx topology used by S3CompatTest, then measure:
# - missing-key GET requests/sec
# - PUT/GET/DELETE throughput for representative object sizes
# - p50/p95/p99 latency and error counts
# - master RSS while concurrent writes approach MINIKV_MAX_BODY_SIZE
```

Record hardware, filesystem, object size mix, replica count, subvolume count,
worker count, volume connection pool width, `MINIKV_MAX_BODY_SIZE`, and volume
paths with every benchmark so future changes are comparable.

Local benchmark snapshot from this repo:

```text
Date: 2026-07-02
Machine: MacBook Pro Mac16,8, Apple M4 Pro, 14 cores, 24 GB RAM
OS: macOS 26.5.1 (25F80)
Topology: one local master, five local nginx/WebDAV volumes, replicas=3,
          subvolumes=10, release build, MINIKV_WORKERS=0,
          MINIKV_VOLUME_POOL=8, MINIKV_PARALLEL_REPLICAS=false
Upstream baseline: geohot/minikeyvalue 451d248, same local nginx topology
```

Comparable commands:

```bash
wrk -t2 -c100 -d10s --latency http://127.0.0.1:$PORT/key
go run tools/thrasher.go -base http://127.0.0.1:$PORT -n 10000 -c 16
```

Results:

| Implementation | Missing GET | 10k write/read/delete |
| --- | ---: | ---: |
| C++ rewrite | 173,036 req/s | 1,800/sec |
| Go upstream on same machine | 163,872 req/s | 1,099-1,191/sec |
| Previously reported upstream reference | 116,338 req/s | 3,815/sec |

Interpretation: on this MacBook, the C++ rewrite is ahead of the same-machine
Go baseline for both tested workloads. It has not reproduced the earlier
3,815/sec upstream thrasher number, so do not treat that external number as a
direct apples-to-apples regression target without matching hardware, OS,
filesystem, nginx config, and process limits.

Interpret benchmark failures carefully. With client-side keep-alive disabled,
long short-connection runs can become a TCP TIME_WAIT / ephemeral-port
benchmark before they reveal the service's true CPU or disk limit. Track:

```bash
netstat -an | rg 'TIME_WAIT|CLOSE_WAIT' | wc -l
```

Production canary recommendation:

- Start with write concurrency at or below 8.
- Test concurrency 16 only after TIME_WAIT, 5xx, fd count, RSS, and p99 latency
  stay stable.
- Treat concurrency 32/64 write failures as a red line until the benchmark
  client uses connection reuse and `MINIKV_VOLUME_POOL` has been sized from
  measured p99/fd/TIME_WAIT data.
- Keep `MINIKV_PARALLEL_REPLICAS=false` unless you are running a controlled
  experiment with connection metrics.

## Release Build

Configure release build:

```bash
cmake --preset release
```

Build release:

```bash
cmake --build --preset release
```

## clangd

The project writes compile commands into the preset build directory. The
checked-in `.clangd` points clangd at:

```text
build/debug
```

If clangd cannot find headers, run:

```bash
cmake --preset debug
```

## Current Layout

```text
include/
  http.hpp
  hash.hpp
  placement.hpp
  record.hpp
  index.hpp
  server.hpp
  volume_client.hpp

src/
  http.cpp
  hash.cpp
  placement.cpp
  record.cpp
  index.cpp
  server.cpp
  volume_client.cpp

tests/
  hash_test.cpp
  http_dependency_test.cpp
  placement_test.cpp
  record_test.cpp
  index_test.cpp
  server_test.cpp
  volume_client_test.cpp
  upstream_compat_test.py
  s3_compat_test.py

deploy/
  minikv-master.env.example
  minikv-master.service
  minikv-volume@.service
  nginx-volume.conf.example
```

## Notes

- `hash.hpp/cpp` wraps MD5 through BoringSSL's OpenSSL-compatible EVP API.
- `index.hpp/cpp` wraps LevelDB for record metadata.
- `http.hpp/cpp` wraps Boost.Beast for master serving and outbound volume
  requests, including custom methods such as `UNLINK` and `REBALANCE`.
- `server.hpp/cpp` holds the app state, key lock table, core write/read/delete
  flows, and thin HTTP route registration.
- `volume_client.hpp/cpp` maps the Go remote access helpers onto the local
  Boost.Beast HTTP adapter.
- `cli.hpp/cpp` parses the Go-style master flags used by `build/debug/mkv`.
- Boost.JSON handles query/list responses and nginx autoindex parsing.
- Tests are registered through GoogleTest and CTest.
- Server and volume client tests start in-process localhost HTTP servers, so
  they need permission to bind loopback ports.
