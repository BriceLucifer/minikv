include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mkv_tests
  record_test.cpp
  placement_test.cpp
  index_test.cpp
  server_test.cpp
)

target_link_libraries(mkv_tests
  PRIVATE
    mkv_core
    GTest::gtest_main
)

target_compile_features(mkv_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(mkv_tests)
