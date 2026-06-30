# TODO

This file tracks the next steps for the C++23 rewrite of `minikeyvalue`.

## Done

- CMake presets for debug/release builds.
- GoogleTest and CTest integration.
- Vendored dependencies through CMake `FetchContent`.
- BoringSSL-backed MD5 helper.
- Base64 encoding helper.
- Record encode/decode logic and tests.
- Placement logic for `key2path`, `key2volume`, and `needs_rebalance`.
- cpp-httplib dependency for future HTTP work.

## Next

1. Implement `LevelDbIndex`.
   - Files: `include/index.hpp`, `src/index.cpp`, `tests/index_test.cpp`
   - Wrap `leveldb::DB` with RAII.
   - Implement `getRecord`, `putRecord`, and `deleteRecord`.
   - Return `Record{{}, Deleted::HARD, ""}` when a key is missing, matching the Go implementation.
   - Add tests for put/get, missing key, delete, and persistence after reopen.

2. Add `AppConfig`.
   - Files: likely `include/server.hpp` or a new `include/app.hpp`.
   - Store volumes, fallback, replicas, subvolumes, protect, md5sum, and volume timeout.
   - Keep it as a simple struct first.

3. Add key lock table.
   - Match Go's `LockKey` and `UnlockKey`.
   - Use `std::mutex` plus `std::unordered_set<std::string>`.
   - Test duplicate lock conflict and unlock behavior.

4. Define `VolumeClient`.
   - Files: `include/volume_client.hpp`, `src/volume_client.cpp`.
   - Interface should cover remote `PUT`, `GET`, `HEAD`, and `DELETE`.
   - Production implementation can use cpp-httplib.
   - Tests should use a fake client first.

5. Implement basic server behavior.
   - Files: `include/server.hpp`, `src/server.cpp`, `tests/server_test.cpp`.
   - Start with `PUT`, `GET`, and `DELETE`.
   - Delay S3 multipart, rebuild, rebalance, list, and unlinked support until the core flow is stable.

6. Add end-to-end smoke test later.
   - Start volume servers.
   - Start C++ master.
   - Use curl or pytest to verify PUT, GET, and DELETE.

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
