include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP true
)

# Keep CRT settings consistent on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Build the test executable containing all test files
add_executable(backtester_tests
        test_loader.cpp
        test_short_call.cpp
        test_position.cpp
        test_diagnostics.cpp
        ../include/data/monthly_aggregate.hpp
        ../src/data/monthly_aggregate.cpp
)

# Target name must match 'backtest_core' defined in root CMakeLists.txt
target_link_libraries(backtester_tests
        PRIVATE
        backtest_core
        GTest::gtest_main
)

if (MINGW)
    target_link_options(backtester_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Auto-discover GoogleTest test cases in CLion's test runner
include(GoogleTest)
gtest_discover_tests(backtester_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)