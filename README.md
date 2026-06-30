# minikv

C++20 rewrite scaffold for `geohot/minikeyvalue`.

The project uses CMake presets and fetches third-party dependencies during
configure. You do not need to install GoogleTest, LevelDB, BoringSSL, or
cpp-httplib with `apt`.

## Dependencies

Required locally:

- CMake 3.20+
- A C++20 compiler
- Git

Fetched by CMake:

- GoogleTest
- LevelDB
- BoringSSL
- cpp-httplib

## Build And Test

Configure debug build:

```bash
cmake --preset debug
```

Build debug:

```bash
cmake --build --preset debug
```

Run tests:

```bash
ctest --preset debug
```

Run the test binary directly:

```bash
./build/debug/tests/mkv_tests
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
```

## Notes

- `hash.hpp/cpp` wraps MD5 through BoringSSL's OpenSSL-compatible EVP API.
- `cpp-httplib` is available for future HTTP client/server work.
- LevelDB is linked into `mkv_core` for the future index wrapper.
- Tests are registered through GoogleTest and CTest.
