#pragma once
#include <cstdio>
#include <cstdlib>

// Keep failure details in the test console and return an error without a Windows crash dialog.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)
