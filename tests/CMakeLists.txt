include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(backtester_tests test_loader.cpp)
target_link_libraries(backtester_tests PRIVATE backtester_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(backtester_tests)