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

// Choice parameters take a normalised value, and the fraction that lands on a
// given index moves whenever an option is added -- which is exactly how a
// two-way switch grown to three silently redirects every existing call. Name
// the index and let the parameter do the arithmetic.
//
// A template only so that this header stays free of the plugin's own includes:
// the engine test wants the DSP core on its own.
template <typename Processor>
void setChoice (Processor& p, const char* id, int index)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
}
