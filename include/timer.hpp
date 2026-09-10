//
// Created by devanandan on 10-09-2026.
//

#pragma once

#include <iostream>
#include <chrono>

struct Timer {
    std::string_view label;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

    explicit Timer(std::string_view name) : label(name) {}

    ~Timer() {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << label << " took " << ms << " ms\n";
    }
};