// Self-check for the SideCrush DSP core. No framework, no fixtures -- just the
// smallest set of assertions that fail if the algorithm in SPEC.md section 1
// stops being true. Build and run it: it prints nothing and exits 0 on success.

#include "SideCrushEngine.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace sidecrush;

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

//==============================================================================
// WTF's intensity dial, at the three settings that are supposed to mean
// something -- SPEC 4.5. The claim the whole feature rests on is the one at
// 100%: the two channels come apart in stereo but their sum is bit-for-bit what
// MONO would have produced, so the effect survives a mono speaker instead of
// half-cancelling on it.
//
// Every case runs both filter positions and both CLIP modes: the correction is
// applied after the lid, so it must not care which of the two the lid produced,
// and POST reaches the mono detector through the filter's linearity rather than
// directly, which is the part most likely to break.
void wtfIntensityEndpoints()
{
    for (const auto post : { false, true })
    for (const auto clip : { false, true })
    {
        const auto params = [post, clip] (float intensity, bool wtf)
        {
            auto p = baseParams();
            p.wtf = wtf;
            p.wtfIntensity = intensity;
            p.clip = clip;
            p.filterPost = post;
            p.ceilingLin = 0.5f;
            p.filterHz = post ? 400.0f : 0.0f;
            return p;
        };

        // A sub on the sidechain and a louder tone on the carrier, panned hard
        // enough that the two channels are genuinely different signals -- a mono
        // carrier would let a wrong per-channel correction pass unnoticed.
        const auto carrierL = [] (int i)
        { return 0.9f * (float) std::sin (2.0 * std::numbers::pi * 700.0 * i / 48000.0); };

        const auto carrierR = [] (int i)
        { return 0.4f * (float) std::sin (2.0 * std::numbers::pi * 1100.0 * i / 48000.0 + 1.0); };

        const auto sidechain = [] (int i)
        { return 0.8f * (float) std::sin (2.0 * std::numbers::pi * 60.0 * i / 48000.0); };

        constexpr int samples = 4800;

        auto zero = makeEngine (params (0.0f, true));
        auto half = makeEngine (params (0.5f, true));
        auto full = makeEngine (params (1.0f, true));
        auto mono = makeEngine (params (0.5f, false)); // wtf off = the MONO link
        auto wide = makeEngine (params (0.5f, true));  // for the per-channel path

        auto zeroGap = 0.0f, fullGap = 0.0f;

        for (int i = 0; i < samples; ++i)
        {
            const auto sc = sidechain (i);

            // 0% -- both channels see the whole rectified sum, which is exactly
            // what the MONO link does. The reference here is the ordinary
            // per-channel path with WTF off, so this also checks that the pair
            // entry point and the single-channel one agree where they overlap.
            auto zl = carrierL (i), zr = carrierR (i);
            zero.processWtfPair (zl, zr, sc);

            const auto ml = mono.processSample (0, carrierL (i), sc);
            const auto mr = mono.processSample (1, carrierR (i), sc);

            CHECK (near (zl, ml));
            CHECK (near (zr, mr));

            // The split is a property of the lids, not of the outputs: the two
            // carriers are different signals, so their outputs differ in every
            // mode and measuring them would say nothing about the split.
            zeroGap = std::fmax (zeroGap, std::fabs (zero.lastLid (0) - zero.lastLid (1)));

            // 50% -- the original WTF, sample for sample. A session saved before
            // this dial existed has to sound the same, and the per-channel path
            // is what it sounded like.
            auto hl = carrierL (i), hr = carrierR (i);
            half.processWtfPair (hl, hr, sc);

            const auto wl = wide.processSample (0, carrierL (i), sc);
            const auto wr = wide.processSample (1, carrierR (i), sc);

            CHECK (near (hl, wl));
            CHECK (near (hr, wr));

            // 100% -- the sum is the mono effect exactly, and the channels are
            // still nothing like each other.
            auto fl = carrierL (i), fr = carrierR (i);
            full.processWtfPair (fl, fr, sc);

            CHECK (near (fl + fr, ml + mr));
            fullGap = std::fmax (fullGap, std::fabs (full.lastLid (0) - full.lastLid (1)));
        }

        // 0% has to collapse the split entirely -- one lid, both channels, which
        // is what makes it MONO. 100% keeps it as wide as WTF ever gets it.
        CHECK (near (zeroGap, 0.0f));
        CHECK (fullGap > 0.5f);
    }
}

//==============================================================================
// The top of the dial widens the side the effect invented. Two things have to
// hold at once, and they pull in opposite directions: the mono sum must still
// not move at all, and material the lid is not touching must come through
// untouched -- otherwise this is just a widener bolted to the output, which
// would wreck an already-stereo input the moment the plugin was idling.
void wtfWidthLeavesMonoAndDryAlone()
{
    for (const auto post : { false, true })
    for (const auto clip : { false, true })
    {
        const auto params = [post, clip] (float intensity)
        {
            auto p = baseParams();
            p.wtf = true;
            p.wtfIntensity = intensity;
            p.clip = clip;
            p.ceilingLin = 0.35f;
            p.filterPost = post;
            p.filterHz = post ? 200.0f : 0.0f;
            p.poles = post ? 4 : 2;
            return p;
        };

        const auto carrierL = [] (int i)
        { return 0.9f * (float) std::sin (2.0 * std::numbers::pi * 700.0 * i / 48000.0); };

        const auto carrierR = [] (int i)
        { return 0.4f * (float) std::sin (2.0 * std::numbers::pi * 1100.0 * i / 48000.0 + 1.0); };

        auto wide = makeEngine (params (1.0f));  // 100%: mid replaced, side doubled
        auto plain = makeEngine (params (0.5f)); // 50%: neither
        auto idle = makeEngine (params (1.0f));

        // Side over mid, in energy -- the width metric that means anything here.
        // A peak-difference measure would just report the two carriers being
        // different signals, which is true in every mode.
        auto sideWide = 0.0, midWide = 0.0, sidePlain = 0.0, midPlain = 0.0;

        // The loudest sample either channel reaches, against the 0.9 the carrier
        // goes in at -- see the peak claim below.
        auto peak = 0.0f;

        for (int i = 0; i < 4800; ++i)
        {
            const auto sc = 0.9f * (float) std::sin (2.0 * std::numbers::pi * 60.0 * i / 48000.0);

            auto wl = carrierL (i), wr = carrierR (i);
            wide.processWtfPair (wl, wr, sc);

            auto pl = carrierL (i), pr = carrierR (i);
            plain.processWtfPair (pl, pr, sc);

            peak = std::fmax (peak, std::fmax (std::fabs (wl), std::fabs (wr)));

            sideWide += 0.25 * (double) (wl - wr) * (wl - wr);
            midWide += 0.25 * (double) (wl + wr) * (wl + wr);
            sidePlain += 0.25 * (double) (pl - pr) * (pl - pr);
            midPlain += 0.25 * (double) (pl + pr) * (pl + pr);

            // Nothing on the sidechain, so the lid never leaves 1 and there is
            // no invented side to scale. The width has to be a no-op here.
            auto il = carrierL (i), ir = carrierR (i);
            idle.processWtfPair (il, ir, 0.0f);

            CHECK (near (il, carrierL (i)));
            CHECK (near (ir, carrierR (i)));
        }

        // It does actually widen, and by a lot -- this is the whole reason the
        // ramp is there. (wtfIntensityEndpoints already holds the other half of
        // the claim: that none of this moves the mono sum.)
        CHECK (std::sqrt (sideWide / midWide) > 1.5 * std::sqrt (sidePlain / midPlain));

        // And what the widening costs in headroom -- SPEC 4.5. At 100% the pair
        // is out_L = dL*lL + dR*(lM - lR), out_R = dR*lR + dL*(lM - lL), so the
        // peak can grow by at most max(lL + |lR - lM|, lR + |lL - lM|).
        //
        // In PRE that factor is 1 at every sample and the peak cannot move at
        // all: one half of the split is always zero, so one lid is wide open and
        // the other IS lM, which leaves a convex combination of the two
        // carriers. That is the invariant worth pinning -- it is a property of
        // the detector maths, so a change there would break it silently.
        //
        // POST unties the three lids and the factor drifts off 1, as far as 2.
        // Not corrected, only bounded: all three lids are in [0, 1], so double
        // is the most it can ever be, and a POST run that came back above that
        // would mean the arithmetic itself had gone wrong rather than the
        // detector merely disagreeing with itself.
        CHECK (peak <= (post ? 2.0f : 1.0f) * 0.9f + tol);
    }
}

//==============================================================================
// RECTI clips only the half of the carrier the sidechain's polarity points at,
// and leaves the other one at full amplitude -- SPEC 4.7.
void rctfClipsOneSideOnly()
{
    for (const auto post : { false, true })
    {
        auto p = baseParams();
        p.clip = true;
        p.ceilingLin = 0.5f;
        p.recti = true;
        p.rectiBlend = 0.0f;   // the midpoint of MIX: the rectified result alone
        p.filterPost = post;

        // POST has to follow the two half-waves separately or the polarity is
        // gone by the time the lid is computed. A short release is what makes
        // that visible: a long one settles both envelopes onto the same number
        // and RECTI degenerates back to the symmetric clip.
        p.filterHz = post ? 8000.0f : 0.0f;

        auto e = makeEngine (p);

        // A sidechain hard over on one side, held long enough for POST's
        // envelope to get there, then hard over on the other.
        const auto settle = [&e] (float sc)
        {
            for (int i = 0; i < 4000; ++i)
                e.processSample (0, 0.0f, sc);
        };

        settle (1.0f);

        // The ceiling is 0.5 and the sidechain is well past it, so the lid on
        // the driven side is shut. The other side never saw a thing.
        CHECK (near (e.processSample (0, 1.0f, 1.0f), 0.0f, 0.02f));
        CHECK (near (e.processSample (0, -1.0f, 1.0f), -1.0f, 0.02f));

        settle (-1.0f);

        CHECK (near (e.processSample (0, -1.0f, -1.0f), 0.0f, 0.02f));
        CHECK (near (e.processSample (0, 1.0f, -1.0f), 1.0f, 0.02f));

        // The two aperture edges are what the scope draws, and they have to
        // disagree for any of the above to be visible.
        CHECK (e.lastLidTop (0) > 0.9f);
        CHECK (e.lastLidBot (0) < 0.1f);
    }
}

//==============================================================================
// The top of MIX's travel is the symmetric result exactly, and RECTI switched
// off is the symmetric result at every point of it -- so nothing that predates
// this parameter moves.
void rctfEndpointsAndBypass()
{
    for (const auto clip : { false, true })
    {
        for (const auto post : { false, true })
        {
            auto p = baseParams();
            p.clip = clip;
            p.ceilingLin = 0.4f;
            p.floorLin = 0.05f;
            p.filterPost = post;
            p.filterHz = 200.0f;
            p.shape = 0.3f;

            auto plain = makeEngine (p);

            auto blended = p;
            blended.recti = true;
            blended.rectiBlend = 1.0f;   // MIX at the top: no rectified content
            auto top = makeEngine (blended);

            auto bypassed = p;
            bypassed.recti = false;
            bypassed.rectiBlend = 0.0f;  // must be ignored outright
            auto off = makeEngine (bypassed);

            for (int i = 0; i < 2000; ++i)
            {
                const auto phase = std::numbers::pi_v<float> * 2.0f * 220.0f
                                 * (float) i / 48000.0f;
                const auto sc = std::sin (phase);
                const auto carrier = std::sin (phase * 3.7f);

                const auto want = plain.processSample (0, carrier, sc);
                CHECK (near (top.processSample (0, carrier, sc), want));
                CHECK (near (off.processSample (0, carrier, sc), want));
            }
        }
    }
}

//==============================================================================
// The mono sum still cancels back to exactly the MONO result at 100% WTF
// intensity, RECTI's asymmetric shape included -- the correction is defined as
// a difference of sums, so it does not care what shape produced them.
void rctfSurvivesWtfCancellation()
{
    auto p = baseParams();
    p.clip = true;
    p.ceilingLin = 0.35f;
    p.wtf = true;
    p.wtfIntensity = 1.0f;
    p.recti = true;
    p.rectiBlend = 0.0f;
    auto wtf = makeEngine (p);

    auto monoParams = p;
    monoParams.wtf = false;
    auto mono = makeEngine (monoParams);

    auto worst = 0.0f;

    for (int i = 0; i < 2000; ++i)
    {
        const auto phase = std::numbers::pi_v<float> * 2.0f * 90.0f * (float) i / 48000.0f;
        const auto sc = std::sin (phase);
        auto l = std::sin (phase * 2.1f);
        auto r = std::sin (phase * 3.3f);

        const auto wantL = mono.processSample (0, l, sc);
        const auto wantR = mono.processSample (0, r, sc);

        wtf.processWtfPair (l, r, sc);
        worst = std::fmax (worst, std::fabs ((l + r) - (wantL + wantR)));
    }

    CHECK (worst <= tol);
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
    wtfIntensityEndpoints();
    wtfWidthLeavesMonoAndDryAlone();
    rctfClipsOneSideOnly();
    rctfEndpointsAndBypass();
    rctfSurvivesWtfCancellation();

    std::puts ("sidecrush engine: all checks passed");
    return 0;
}
