cmake_minimum_required(VERSION 3.20)
project(options_backtest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(backtest_core
        src/data/loader.cpp
        src/strategy/short_call.cpp
        src/strategy/position.cpp
        src/strategy/diagnostics.cpp
        src/io/json_out.cpp
        include/timer.hpp
        include/strategy/short_call.hpp
        include/io/json_out.hpp
        include/data/monthly_aggregate.hpp
        src/data/monthly_aggregate.cpp
        include/data/equity_loader.hpp
        src/data/equity_loader.cpp
)

target_include_directories(backtest_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Applied to every executable. A binary built without these runs fine from an IDE
# but dies under a subprocess call with a missing-DLL dialog and no stderr.
function(apply_static_link_options target)
    if (MINGW)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endfunction()

add_executable(backtest_main src/main.cpp)
target_link_libraries(backtest_main PRIVATE backtest_core)
apply_static_link_options(backtest_main)

add_executable(backtest_cli src/cli/main_cli.cpp)
target_link_libraries(backtest_cli PRIVATE backtest_core)
apply_static_link_options(backtest_cli)

# Only the CLI gets a fixed location, so the web server can find it without
# guessing between generator layouts. The other targets stay where they were:
# backtest_main resolves its data paths relative to the build dir, and the test
# runner is launched from there too.
set_target_properties(backtest_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()
add_subdirectory(tests)
