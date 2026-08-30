// Self-check for the HardCap DSP core. No framework, no fixtures -- just the
// smallest set of assertions that fail if the algorithm in SPEC.md section 1
// stops being true. Build and run it: it prints nothing and exits 0 on success.

#include "HardCapEngine.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace hardcap;

namespace
{

constexpr float tol = 1.0e-4f;

bool near (float a, float b, float t = tol) { return std::fabs (a - b) <= t; }

// Everything not named here is Params' own default, which the tests then
// exercise as a side effect of using them.
Params baseParams()
{
    Params p;
    p.ceilingLin = 1.0f;
    p.clip = false;
    return p;
}

Engine makeEngine (const Params& p)
{
    Engine e;
    e.prepare (48000.0, 2);
    e.setParams (p);
    return e;
}

//==============================================================================
void windowEndpoints()
{
    auto p = baseParams();
    p.ceilingLin = 0.5f;
    p.floorLin = 0.1f;
    auto e = makeEngine (p);

    // Below the floor the lid is wide open: carrier passes untouched.
    CHECK (near (e.processSample (0, 1.0f, 0.05f), 1.0f));

    // At and above the ceiling the lid is shut: silence, whatever the carrier.
    CHECK (near (e.processSample (0, 1.0f, 0.5f), 0.0f));
    CHECK (near (e.processSample (0, 1.0f, 9.0f), 0.0f));

    // Halfway across the window at SHAPE 0 is a linear half.
    CHECK (near (e.processSample (0, 1.0f, 0.3f), 0.5f));
}

//==============================================================================
void shapeBreaksLateAndEarly()
{
    const auto lidAtHalfway = [] (float shape)
    {
        auto p = baseParams();
        p.shape = shape;
        auto e = makeEngine (p);
        return e.processSample (0, 1.0f, 0.5f); // carrier 1.0 in VCA mode == the lid
    };

    const auto late = lidAtHalfway (-1.0f);
    const auto linear = lidAtHalfway (0.0f);
    const auto early = lidAtHalfway (1.0f);

    CHECK (late > 0.99f);          // -1: still wide open halfway across
    CHECK (near (linear, 0.5f));   //  0: exactly halfway shut
    CHECK (early < 0.10f);         // +1: already slammed shut

    // Monotonic in shape: later shape always means a more open lid.
    CHECK (late > linear && linear > early);
}

//==============================================================================
// The claim the whole plugin rests on: a clamp flattens where a multiply scales.
void clipFlattensVcaScales()
{
    auto p = baseParams();
    p.shape = 0.0f;
    p.ceilingLin = 1.0f;

    // Sidechain at 0.5 -> lid 0.5. Two carrier values, both above the lid.
    auto vca = makeEngine (p);
    const auto vcaLoud = vca.processSample (0, 0.9f, 0.5f);
    const auto vcaQuiet = vca.processSample (0, 0.6f, 0.5f);

    p.clip = true;
    auto clip = makeEngine (p);
    const auto clipLoud = clip.processSample (0, 0.9f, 0.5f);
    const auto clipQuiet = clip.processSample (0, 0.6f, 0.5f);

    // VCA: both scaled by the same factor, so their ratio survives.
    CHECK (near (vcaLoud, 0.45f));
    CHECK (near (vcaQuiet, 0.30f));
    CHECK (near (vcaLoud / vcaQuiet, 0.9f / 0.6f));

    // Clip: both railed to the lid. The difference between them is gone --
    // that is the detail destruction, and no gain curve can reproduce it.
    CHECK (near (clipLoud, 0.5f));
    CHECK (near (clipQuiet, 0.5f));
    CHECK (near (clipLoud, clipQuiet));

    // A carrier below the lid is untouched by the clamp but still scaled by
    // the multiply. This is the carrier-level dependence the VCA path lacks.
    auto clip2 = makeEngine (p);
    CHECK (near (clip2.processSample (0, 0.2f, 0.5f), 0.2f));
}

//==============================================================================
void floorCannotCrossCeiling()
{
    auto p = baseParams();
    p.ceilingLin = 0.25f;
    p.floorLin = 0.9f; // deliberately above the ceiling
    auto e = makeEngine (p);

    CHECK (e.getParams().floorLin < e.getParams().ceilingLin);

    // The window still behaves: open below, shut at the ceiling, finite between.
    const auto low = e.processSample (0, 1.0f, 0.0f);
    const auto high = e.processSample (0, 1.0f, 1.0f);
    CHECK (near (low, 1.0f));
    CHECK (near (high, 0.0f));
    CHECK (std::isfinite (low) && std::isfinite (high));
}

//==============================================================================
void rectifierGivesTwoClosuresPerCycle()
{
    auto p = baseParams();
    p.ceilingLin = 0.8f;
    auto e = makeEngine (p);

    constexpr double sr = 48000.0;
    constexpr double freq = 50.0;
    const auto samplesPerCycle = (int) (sr / freq);

    int closures = 0;
    auto wasOpen = true;

    for (int i = 0; i < samplesPerCycle; ++i)
    {
        const auto sc = (float) std::sin (2.0 * std::numbers::pi * freq * i / sr);
        e.processSample (0, 1.0f, sc);
        const auto shut = e.lastLid (0) < 0.4f;

        if (shut && wasOpen)
            ++closures;

        wasOpen = ! shut;
    }

    // A rectified detector closes on both peaks, matching a real clipper.
    CHECK (closures == 2);
}

//==============================================================================
void filterIsAStableLowpass()
{
    for (int poles = 1; poles <= 8; ++poles)
    {
        DetectorFilter f;
        f.setup (200.0f, poles, 48000.0);
        f.reset();

        // DC settles at unity.
        float dc = 0.0f;
        for (int i = 0; i < 20000; ++i)
            dc = f.process (1.0f);

        CHECK (near (dc, 1.0f, 0.02f));

        // A tone well above cutoff is attenuated, and more so with more poles.
        f.reset();
        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const auto x = (float) std::sin (2.0 * std::numbers::pi * 5000.0 * i / 48000.0);
            const auto y = f.process (x);

            if (i > 10000)
                peak = std::max (peak, std::fabs (y));
        }

        CHECK (peak < 0.2f);
        CHECK (std::isfinite (peak));
    }

    // The awkward corner: 20 Hz, 8 poles, 44.1 kHz. Biquads misbehave here.
    DetectorFilter deep;
    deep.setup (20.0f, 8, 44100.0);
    deep.reset();

    for (int i = 0; i < 200000; ++i)
    {
        const auto y = deep.process ((float) std::sin (2.0 * std::numbers::pi * 1000.0 * i / 44100.0));
        CHECK (std::isfinite (y));
        CHECK (std::fabs (y) < 10.0f);
    }
}

//==============================================================================
void postRectifyEnvelopeStaysValid()
{
    auto p = baseParams();
    p.filterPost = true;
    p.filterHz = 30.0f;
    p.poles = 8; // maximum ringing
    p.ceilingLin = 0.5f;
    auto e = makeEngine (p);

    for (int i = 0; i < 100000; ++i)
    {
        const auto sc = i % 4000 < 200 ? 1.0f : 0.0f; // hard bursts, worst case
        e.processSample (0, 1.0f, sc);
        const auto lid = e.lastLid (0);

        // Butterworth overshoot must never push the lid outside [0,1], which
        // would mean boosting the carrier or a negative base in the exponent.
        CHECK (lid >= 0.0f && lid <= 1.0f);
        CHECK (std::isfinite (lid));
    }
}

//==============================================================================
// WTF gives the left channel the sidechain's positive half and the right its
// negative half, so a sub going up shuts one side and going down shuts the
// other. The panning is the whole point, so check the channels actually
// disagree -- and that POST, where the rectifier could throw the sign away
// before the split, still disagrees the same way.
void wtfSplitsBySign()
{
    for (const auto post : { false, true })
    {
        auto p = baseParams();
        p.wtf = true;
        p.ceilingLin = 1.0f;
        p.filterPost = post;
        p.filterHz = 0.0f; // filter off: the split is what is under test, not it
        auto e = makeEngine (p);

        // Same summed sidechain into both channels, as processBlock feeds it.
        const auto left = e.processSample (0, 1.0f, 0.8f);
        const auto right = e.processSample (1, 1.0f, 0.8f);

        // A positive sidechain closes the left and leaves the right wide open.
        CHECK (near (left, 0.2f));
        CHECK (near (right, 1.0f));

        const auto leftDown = e.processSample (0, 1.0f, -0.8f);
        const auto rightDown = e.processSample (1, 1.0f, -0.8f);

        // And a negative one does the opposite. Neither is what STEREO would do,
        // which rectifies and shuts both.
        CHECK (near (leftDown, 1.0f));
        CHECK (near (rightDown, 0.2f));
    }

    // POST filtering has to happen on the two halves separately: a shared
    // envelope would move both channels together and there would be no pan left.
    auto p = baseParams();
    p.wtf = true;
    p.ceilingLin = 1.0f;
    p.filterPost = true;
    p.filterHz = 200.0f;
    p.poles = 2;
    auto e = makeEngine (p);

    auto widest = 0.0f;

    for (int i = 0; i < 4800; ++i) // 100 cycles of a 100 Hz sine at 48 kHz
    {
        const auto sc = 0.9f * (float) std::sin (2.0 * std::numbers::pi * 100.0 * i / 48000.0);
        e.processSample (0, 1.0f, sc);
        const auto l = e.lastLid (0);
        e.processSample (1, 1.0f, sc);
        widest = std::fmax (widest, std::fabs (l - e.lastLid (1)));
    }

    CHECK (widest > 0.2f);
}

} // namespace

int main()
{
    windowEndpoints();
    shapeBreaksLateAndEarly();
    clipFlattensVcaScales();
    floorCannotCrossCeiling();
    rectifierGivesTwoClosuresPerCycle();
    filterIsAStableLowpass();
    postRectifyEnvelopeStaysValid();
    wtfSplitsBySign();

    std::puts ("hardcap engine: all checks passed");
    return 0;
}
