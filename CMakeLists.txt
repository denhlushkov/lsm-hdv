cmake_minimum_required(VERSION 3.20)

project(lsm_hdv
  VERSION 0.2.0
  DESCRIPTION "Embedded LSM-tree key-value storage engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LSM_BUILD_TESTS "Build the test suite" ON)
option(LSM_WITH_RAPIDCHECK "Build property-based tests (RapidCheck)" ON)
option(LSM_WERROR "Treat compiler warnings as errors" OFF)
set(LSM_SANITIZE "" CACHE STRING
  "Comma separated list of sanitizers (e.g. address,undefined or thread)")

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
add_library(lsm_compile_options INTERFACE)
if(MSVC)
  target_compile_options(lsm_compile_options INTERFACE /W4 /permissive-)
  if(LSM_WERROR)
    target_compile_options(lsm_compile_options INTERFACE /WX)
  endif()
else()
  target_compile_options(lsm_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  if(LSM_WERROR)
    target_compile_options(lsm_compile_options INTERFACE -Werror)
  endif()
  if(LSM_SANITIZE)
    target_compile_options(lsm_compile_options INTERFACE
      -fsanitize=${LSM_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(lsm_compile_options INTERFACE -fsanitize=${LSM_SANITIZE})
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(lsm STATIC
  src/util/arena.cc
  src/util/coding.cc
  src/util/comparator.cc
  src/util/crc32.cc
  src/util/hash.cc
  src/util/iterator.cc
  src/util/status.cc
  src/env/env.cc
  src/env/env_std.cc
  src/env/mem_env.cc
  src/db/dbformat.cc
  src/db/filename.cc
)
target_include_directories(lsm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm
  PUBLIC  Threads::Threads
  PRIVATE lsm_compile_options)

# Internal headers are needed by white-box tests.
add_library(lsm_internal INTERFACE)
target_include_directories(lsm_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm_internal INTERFACE lsm)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
