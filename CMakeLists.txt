cmake_minimum_required(VERSION 3.20)
project(options_backtest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(backtest_core
        src/data/loader.cpp
)
target_include_directories(backtest_core PUBLIC include)

add_executable(backtest_main src/main.cpp)
target_link_libraries(backtest_main PRIVATE backtest_core)

if (MINGW)
    target_link_options(backtest_main PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_subdirectory(tests)