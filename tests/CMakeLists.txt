include(FetchContent)

# GoogleTest v1.18.0, pinned by release tarball hash. Downloaded into the
# build tree (<build>/_deps).
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Same sanitizer instrumentation as our code, none of our warnings.
satsim_enable_sanitizers(gtest)
satsim_enable_sanitizers(gtest_main)

include(GoogleTest)

add_executable(satsim_core_tests
  core/angles_test.cpp)
target_link_libraries(satsim_core_tests PRIVATE satsim::core GTest::gtest_main)
satsim_set_warnings(satsim_core_tests)
satsim_enable_sanitizers(satsim_core_tests)
gtest_discover_tests(satsim_core_tests)
