#pragma once

#include <cstdio>
#include <cstdlib>

// assert() vanishes under NDEBUG, which every Release build sets -- a self-check
// that disappears in the configuration people actually ship is worse than none.
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (! (cond))                                                                  \
        {                                                                              \
            std::fprintf (stderr, "FAILED  %s:%d\n        %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                              \
        }                                                                              \
    } while (false)
