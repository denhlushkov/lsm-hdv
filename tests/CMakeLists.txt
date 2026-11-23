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
  bloom_filter_test.cc
  lru_cache_test.cc
  block_test.cc
  table_test.cc
  builder_test.cc
)

add_executable(lsm_tests ${LSM_UNIT_TESTS})
target_link_libraries(lsm_tests PRIVATE lsm_internal lsm_compile_options GTest::gtest_main)
# PRE_TEST: list the tests when ctest runs, not as a post-build step, so the
# build never executes (possibly sanitizer-instrumented) test binaries.
gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)

if(LSM_WITH_RAPIDCHECK)
  FetchContent_Declare(rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        2c3c4365aca21ef4e612768fdc95b1ce8b39a651)
  set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
  set(RC_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(rapidcheck)

  add_executable(lsm_property_tests property_test.cc)
  target_link_libraries(lsm_property_tests PRIVATE
    lsm_internal lsm_compile_options rapidcheck rapidcheck_gtest GTest::gtest_main)
  gtest_discover_tests(lsm_property_tests DISCOVERY_TIMEOUT 60
                       DISCOVERY_MODE PRE_TEST)
endif()
