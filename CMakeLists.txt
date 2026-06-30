cmake_minimum_required(VERSION 3.20)

project(minikv
  VERSION 0.1.0
  LANGUAGES CXX
)

include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIKV_BUILD_TESTS "Build minikv tests" ON)

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Avoid pulling optional system dependencies into httplib by default.
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  leveldb
  GIT_REPOSITORY https://github.com/google/leveldb.git
  GIT_TAG main
)

FetchContent_Declare(
  boringssl
  GIT_REPOSITORY https://github.com/google/boringssl.git
  GIT_TAG main
)

FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.48.0
)

FetchContent_MakeAvailable(leveldb)
FetchContent_MakeAvailable(boringssl)
FetchContent_MakeAvailable(cpp_httplib)

if(MINIKV_BUILD_TESTS)
  set(BUILD_TESTING ON CACHE BOOL "" FORCE)
endif()

add_library(mkv_core STATIC
  src/base64.cpp
  src/hash.cpp
  src/record.cpp
  src/placement.cpp
  src/index.cpp
  src/server.cpp
  src/volume_client.cpp
)

target_include_directories(mkv_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(mkv_core PUBLIC cxx_std_23)

target_link_libraries(mkv_core
  PUBLIC
    httplib::httplib
  PRIVATE
    crypto
    leveldb
)

if(MINIKV_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()
