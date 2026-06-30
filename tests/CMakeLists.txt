include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mkv_tests
  base64_test.cpp
  cli_test.cpp
  hash_test.cpp
  http_dependency_test.cpp
  record_test.cpp
  rebalance_test.cpp
  rebuild_test.cpp
  placement_test.cpp
  index_test.cpp
  server_test.cpp
  volume_client_test.cpp
)

target_link_libraries(mkv_tests
  PRIVATE
    mkv_core
    GTest::gtest_main
)

target_compile_features(mkv_tests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(mkv_tests)

add_test(
  NAME NginxSmokeTest
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/nginx_smoke_test.sh $<TARGET_FILE:mkv>
)
