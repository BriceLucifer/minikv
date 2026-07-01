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

Optional compatibility test clients:

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
PyArrow file-info/list/delete/parquet checks. If `boto3` or `pyarrow` are not
installed, those suites are reported as skipped rather than failing the core
build. Set `MINIKV_RUN_LARGE_S3_COMPAT=1` to include the larger multipart
parquet roundtrip.

If a local `.venv` exists at the repository root, the harness uses
`.venv/bin/python3` automatically. This keeps boto3/PyArrow test dependencies
out of the system Python while still exercising the real client libraries.

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
  server
```

Then use the original API shape:

```bash
curl -v -L -X PUT -d hello localhost:3000/hello
curl -v -L localhost:3000/hello
curl -v -L -X DELETE localhost:3000/hello
```

The C++ master also implements the upstream S3 compatibility subset for bucket
listing, bulk delete, and multipart upload:

```bash
curl -s 'localhost:3000/bucket?list-type=2'
curl -X POST -H 'Content-Type: application/xml' \
  --data-binary '<Delete><Object><Key>file.txt</Key></Object></Delete>' \
  'localhost:3000/bucket?delete'

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

One host can run several volume services by copying
`deploy/nginx-volume.conf.example` to `/etc/minikv/volume-1.conf`,
`/etc/minikv/volume-2.conf`, and so on, then changing `listen`, `pid`, log, and
`root` paths. Start them with systemd instance names:

```bash
sudo install -d /etc/minikv /var/lib/minikv
sudo cp deploy/minikv-volume@.service /etc/systemd/system/
sudo cp deploy/nginx-volume.conf.example /etc/minikv/volume-1.conf
sudo systemctl daemon-reload
sudo systemctl enable --now minikv-volume@1
```

Install and start the master:

```bash
sudo install -m 0755 build/release/mkv /usr/local/bin/mkv
sudo cp deploy/minikv-master.env.example /etc/minikv/master.env
sudo cp deploy/minikv-master.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now minikv-master
```

Edit `/etc/minikv/master.env` before starting production. `MINIKV_VOLUMES`
must list every volume address the master should use, and
`MINIKV_REPLICAS` must be no larger than that list. Keep `MINIKV_SUBVOLUMES`
stable for an existing deployment unless you are deliberately rebalancing.

Before accepting traffic, run the same gates used by this repo:

```bash
cmake --build --preset release
ctest --preset debug
MINIKV_REQUIRE_HTTP_COMPAT_DEPS=1 \
  ctest --preset debug -R UpstreamCompatTest --output-on-failure
MINIKV_REQUIRE_S3_COMPAT_DEPS=1 \
  ctest --preset debug -R S3CompatTest --output-on-failure
```

For a machine without system Python packages, install the optional client
libraries into the local virtualenv with `uv`:

```bash
UV_CACHE_DIR=$PWD/.cache/uv uv venv .venv
UV_CACHE_DIR=$PWD/.cache/uv uv pip install requests boto3 pyarrow
```

Operational notes:

- Back up the LevelDB directory and volume files together when possible.
- Use `UNLINK` plus `?unlinked` if you want a protected delete workflow.
- Use `rebuild` to reconstruct LevelDB metadata from volume files after losing
  the index.
- Use `rebalance` after changing the preferred volume list; stop the master
  first because LevelDB allows one process at a time.
- Watch nginx disk usage and the master `*.multipart` scratch directory.
- Keep volume ports private to trusted clients or put authentication/TLS in
  front of the master and volumes.

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
