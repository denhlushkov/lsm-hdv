include(FetchContent)

# Older third-party CMake files declare very old minimum versions which
# CMake >= 4.0 rejects; this keeps them building.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59)  # v1.15.2
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(LSM_UNIT_TESTS
  util_test.cc
  env_test.cc
  arena_skiplist_test.cc
  memtable_test.cc
  wal_test.cc
)

add_executable(lsm_tests ${LSM_UNIT_TESTS})
target_link_libraries(lsm_tests PRIVATE lsm_internal lsm_compile_options GTest::gtest_main)
gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 60)

