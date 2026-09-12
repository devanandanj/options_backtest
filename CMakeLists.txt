cmake_minimum_required(VERSION 3.20)
project(options_backtest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(backtest_core
        src/data/loader.cpp
        src/strategy/short_call.cpp
        include/timer.hpp
        include/strategy/short_call.hpp
        include/data/monthly_aggregate.hpp
        src/data/monthly_aggregate.cpp
        include/data/equity_loader.hpp
        src/data/equity_loader.cpp
)

target_include_directories(backtest_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(backtest_main src/main.cpp)
target_link_libraries(backtest_main PRIVATE backtest_core)

if (MINGW)
    target_link_options(backtest_main PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_subdirectory(tests)