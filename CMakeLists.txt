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

function(minikv_enable_project_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target}
        PRIVATE
          -Wmissing-designated-field-initializers
      )
    endif()
  elseif(MSVC)
    target_compile_options(${target}
      PRIVATE
        /W4
    )
  endif()
endfunction()

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BOOST_INCLUDE_LIBRARIES asio beast json CACHE STRING "" FORCE)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  leveldb
  GIT_REPOSITORY https://github.com/google/leveldb.git
  GIT_TAG 7ee830d02b623e8ffe0b95d59a74db1e58da04c5
)

FetchContent_Declare(
  boringssl
  GIT_REPOSITORY https://github.com/google/boringssl.git
  GIT_TAG e3ca87bd0c240ef7d142ced16ddf87889ffc622e
)

FetchContent_Declare(
  boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(leveldb)
FetchContent_MakeAvailable(boringssl)
FetchContent_MakeAvailable(boost)

if(MINIKV_BUILD_TESTS)
  set(BUILD_TESTING ON CACHE BOOL "" FORCE)
endif()

add_library(mkv_core STATIC
  src/base64.cpp
  src/hash.cpp
  src/record.cpp
  src/placement.cpp
  src/index.cpp
  src/http.cpp
  src/rebalance.cpp
  src/rebuild.cpp
  src/server.cpp
  src/volume_client.cpp
  src/cli.cpp
)

target_include_directories(mkv_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(mkv_core PUBLIC cxx_std_23)
minikv_enable_project_warnings(mkv_core)

target_link_libraries(mkv_core
  PUBLIC
    Boost::asio
    Boost::beast
    Boost::json
  PRIVATE
    crypto
    leveldb
)

add_executable(mkv
  src/main.cpp
)

target_link_libraries(mkv
  PRIVATE
    mkv_core
)

target_compile_features(mkv PRIVATE cxx_std_23)
minikv_enable_project_warnings(mkv)

if(MINIKV_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()
