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
configure. You do not need to install GoogleTest, LevelDB, BoringSSL, or
cpp-httplib with `apt`.

## Dependencies

Required locally:

- CMake 3.20+
- A C++23 compiler
- Git

Fetched by CMake:

- GoogleTest
- LevelDB
- BoringSSL
- cpp-httplib
- nlohmann/json

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
  hash.hpp
  placement.hpp
  record.hpp
  index.hpp
  server.hpp
  volume_client.hpp

src/
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
- `server.hpp/cpp` holds the app state, key lock table, core write/read/delete
  flows, and thin HTTP route registration.
- `volume_client.hpp/cpp` maps the Go remote access helpers onto cpp-httplib.
- `cli.hpp/cpp` parses the Go-style master flags used by `build/debug/mkv`.
- `nlohmann/json` is available for upcoming HTTP query/response handling.
- Tests are registered through GoogleTest and CTest.
- Server and volume client tests start in-process localhost HTTP servers, so
  they need permission to bind loopback ports.
