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
  server
```

Then use the original API shape:

```bash
curl -v -L -X PUT -d hello localhost:3000/hello
curl -v -L localhost:3000/hello
curl -v -L -X DELETE localhost:3000/hello
```

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
