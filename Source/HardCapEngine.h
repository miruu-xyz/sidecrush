#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// HardCap DSP core.
//
// Deliberately free of JUCE types so the whole signal path can be exercised by
// tests/engine_test.cpp without a plugin host. See SPEC.md section 1.

namespace hardcap
{

//==============================================================================
// lid = 1 - t^p,  p = clamp(2^(-4*shape), 1/32, 32)
//
// p only changes when SHAPE moves, so the curve lives in a table rather than a
// per-sample pow(): at 8x oversampled stereo that would be ~770k calls/second
// and would dominate the entire plugin.
class ShapeTable
{
public:
    static constexpr int size = 1024;

    void setShape (float shape)
    {
        // SHAPE has a 0.01 step, so anything smaller is not worth a rebuild --
        // and an exact float compare here trips -Wfloat-equal for no benefit.
        // currentShape starts outside SHAPE's range, so the first call always builds.
        if (std::fabs (shape - currentShape) < 1.0e-6f)
            return;

        currentShape = shape;
        const auto p = std::clamp (std::pow (2.0f, -4.0f * shape), 1.0f / 32.0f, 32.0f);

        for (int i = 0; i <= size; ++i)
            table[(size_t) i] = 1.0f - std::pow ((float) i / (float) size, p);
    }

    // t in [0,1] -> lid in [0,1]. 0 = wide open, 1 = fully shut.
    float lid (float t) const noexcept
    {
        const auto x = t * (float) size;
        const auto i = (int) x;

        if (i >= size)
            return table[(size_t) size];

        const auto f = x - (float) i;
        return table[(size_t) i] + f * (table[(size_t) i + 1] - table[(size_t) i]);
    }

private:
    std::array<float, (size_t) size + 1> table {};
    float currentShape = 1.0e9f;
};

//==============================================================================
// Topology-preserving state variable sections. Not biquads: at 20 Hz with 8
// poles cascaded biquads are numerically fragile at 44.1 kHz, ring badly, and
// dislike being modulated -- and this cutoff is meant to be swept.

struct SvfLowpass
{
    void setCoefficients (float g, float q) noexcept
    {
        const auto k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (float v0) noexcept
    {
        const auto v3 = v0 - ic2;
        const auto v1 = a1 * ic1 + a2 * v3;
        const auto v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0f; }

    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1 = 0.0f, ic2 = 0.0f;
};

struct OnePoleLowpass
{
    void setCoefficients (float g) noexcept { bigG = g / (1.0f + g); }

    float process (float x) noexcept
    {
        const auto v = (x - s) * bigG;
        const auto y = v + s;
        s = y + v;
        return y;
    }

    void reset() noexcept { s = 0.0f; }

    float bigG = 0.0f, s = 0.0f;
};

// Butterworth-aligned cascade, 1..8 poles. Odd orders get the one-pole section.
class DetectorFilter
{
public:
    static constexpr int maxPoles = 8;

    void setup (float cutoffHz, int poles, double sampleRate) noexcept
    {
        poles = std::clamp (poles, 1, maxPoles);

        // A tan and up to four sins, so skip it when nothing moved -- the same
        // guard ShapeTable uses one screen up. lastPoles starts at 0, which poles
        // can never be, so the first call always builds. FILTER has a 1 Hz step,
        // so a millihertz of tolerance costs nothing.
        if (poles == lastPoles
            && std::fabs (cutoffHz - lastCutoffHz) < 1.0e-3f
            && std::fabs (sampleRate - lastSampleRate) < 1.0e-9)
            return;

        lastPoles = poles;
        lastCutoffHz = cutoffHz;
        lastSampleRate = sampleRate;

        const auto nyquistLimit = (float) sampleRate * 0.45f;

        bypassed = (cutoffHz <= 0.0f || cutoffHz >= nyquistLimit);

        if (bypassed)
            return;

        const auto g = std::tan (std::numbers::pi_v<float> * cutoffHz / (float) sampleRate);

        numSections = poles / 2;
        useOnePole = (poles % 2) != 0;

        for (int k = 0; k < numSections; ++k)
        {
            // Q_k = 1 / (2 sin(pi(2k+1) / 2N)) -- standard Butterworth section Qs.
            const auto q = 1.0f / (2.0f * std::sin (std::numbers::pi_v<float> * (float) (2 * k + 1)
                                                    / (float) (2 * poles)));
            sections[(size_t) k].setCoefficients (g, q);
        }

        if (useOnePole)
            onePole.setCoefficients (g);
    }

    float process (float x) noexcept
    {
        if (bypassed)
            return x;

        for (int k = 0; k < numSections; ++k)
            x = sections[(size_t) k].process (x);

        if (useOnePole)
            x = onePole.process (x);

        return x;
    }

    void reset() noexcept
    {
        for (auto& s : sections)
            s.reset();

        onePole.reset();
    }

private:
    std::array<SvfLowpass, (size_t) maxPoles / 2> sections {};
    OnePoleLowpass onePole;
    int numSections = 0;
    bool useOnePole = false;
    bool bypassed = true;

    // Last configuration built, so an unchanged setup() is free. reset() clears
    // the filter state but deliberately not these -- the coefficients still stand.
    float lastCutoffHz = 0.0f;
    double lastSampleRate = 0.0;
    int lastPoles = 0;
};

//==============================================================================
struct Params
{
    float preGain = 1.0f;      // linear
    float ceilingLin = 0.5f;   // linear amplitude
    float floorLin = 0.0f;     // linear amplitude, forced below ceilingLin
    float shape = 0.0f;        // -1 .. +1
    float filterHz = 0.0f;     // <= 0 or >= nyquist*0.45 means off
    int poles = 2;             // 1..8
    bool clip = true;          // true = clip ceiling, false = VCA multiply
    bool filterPost = false;   // rectifier order
    bool wtf = false;          // split the summed sidechain: + shuts L, - shuts R
    float wtfIntensity = 0.5f; // 0 .. 1, how far WTF is taken -- see 4.5
};

class Engine
{
public:
    void prepare (double newSampleRate, int numChannels)
    {
        sampleRate = newSampleRate;
        filters.resize ((size_t) std::max (1, numChannels));
        lids.assign (filters.size(), 1.0f);
        detectors.assign (filters.size(), 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : filters)
            f.reset();

        std::fill (lids.begin(), lids.end(), 1.0f);
        std::fill (detectors.begin(), detectors.end(), 0.0f);
    }

    // HQ switches the oversampling factor, which changes the rate the filter
    // coefficients were designed for. Allocation-free: the vectors keep their size.
    void setSampleRate (double newSampleRate)
    {
        sampleRate = newSampleRate;
        setParams (params);
    }

    // The window can never invert, but it may be as narrow as the controls can
    // ask for: one 0.1 dB step, which is the FLOOR parameter's own interval, so
    // the two thresholds can be set to the nearest thing to identical the dial
    // grid allows. The editor draws the same clamp and the FLOOR readout brackets
    // its value once it bites, so this lives here rather than inside setParams --
    // three copies of the number would drift apart silently.
    static constexpr float floorHeadroomDb = -0.1f;
    static constexpr float floorHeadroom = 0.988553095f; // 10^(-0.1/20)

    static constexpr float clampFloor (float floorLin, float ceilingLin) noexcept
    {
        return std::max (0.0f, std::min (floorLin, ceilingLin * floorHeadroom));
    }

    void setParams (const Params& p)
    {
        params = p;
        params.floorLin = clampFloor (params.floorLin, params.ceilingLin);

        windowScale = 1.0f / std::max (1.0e-9f, params.ceilingLin - params.floorLin);

        shapeTable.setShape (params.shape);

        // WTF's intensity moves two things, one per half of its travel -- SPEC
        // 4.5. Below 50% the detector split opens up, from both channels seeing
        // the whole rectified sum (which is MONO) to each seeing only its own
        // half. Above 50% the split stays open and the *mid* is replaced by what
        // MONO would have produced, which is what makes a mono sum cancel back
        // to the mono effect exactly.
        //
        // Riding the second half with it: the width of the side the effect
        // invents, up to double. It is on the same ramp rather than a dial of
        // its own because it exists to answer the thing replacing the mid costs
        // -- the level difference between the channels, which exact cancellation
        // makes impossible -- so the two belong at the same end of the travel.
        //
        // 50% is the rest position of both, so it is the original WTF to the
        // sample, and a session saved before this dial existed sounds the same.
        const auto intensity = std::clamp (params.wtfIntensity, 0.0f, 1.0f);
        splitAmount = std::min (1.0f, 2.0f * intensity);
        midBlend = std::max (0.0f, 2.0f * intensity - 1.0f);

        for (auto& f : filters)
            f.setup (params.filterHz, params.poles, sampleRate);
    }

    // One sample of one channel. `sc` is the detector input for this channel.
    //
    // A stereo instance in WTF goes through processWtfPair below instead, which
    // needs both channels at once. What is left here is the mono instance, whose
    // WTF is fixed at the 50% behaviour: with one channel there is no second half
    // to hand the negative excursions to and no mid to cancel anything against,
    // so the intensity dial has nothing to move -- SPEC 4.5.
    float processSample (int channel, float carrier, float sc) noexcept
    {
        auto& filter = filters[(size_t) channel];

        // WTF hands both channels the same summed sidechain and gives each one
        // half of it: the left is driven by its positive excursions, the right by
        // its negative ones -- SPEC 4.5. Flipping the right channel's copy turns
        // "the negative half" into "the positive half" and lets one half-wave
        // rectifier serve both sides. A full-wave one would undo the split.
        const auto sign = channel == 0 ? 1.0f : -1.0f;

        // Filter before rectifier keeps the detector at waveform rate. Reversing
        // them turns it into an envelope follower -- see SPEC 4.3.
        // In PRE the filtered signal is still bipolar, which is what the scope
        // draws the symmetric thresholds against. In POST it is already an
        // envelope, and Butterworth ringing can drive it below zero -- clamp so
        // the lid never exceeds 1 and the exponent never sees a negative base.
        float mag;

        if (params.filterPost)
        {
            // The split has to come first here: the rectifier is the thing that
            // throws away the sign the split is made of. Two half-wave signals in
            // means two envelopes out, one per side, which is what POST has to
            // follow for the panning to survive the filter.
            const auto rectified = params.wtf ? std::max (0.0f, sign * sc) : std::abs (sc);
            const auto env = std::max (0.0f, filter.process (rectified));
            detectors[(size_t) channel] = env;
            mag = env;
        }
        else
        {
            // PRE filters the bipolar sidechain, so the sign survives it and the
            // split can happen after -- which is what keeps this at waveform rate.
            // The detector stays bipolar so the scope draws the whole wave: in
            // WTF its top lobe is the left channel and its bottom lobe the right.
            const auto filtered = filter.process (sc);
            detectors[(size_t) channel] = filtered;
            mag = params.wtf ? std::max (0.0f, sign * filtered) : std::abs (filtered);
        }

        const auto lid = lidFor (mag);
        lids[(size_t) channel] = lid;

        // OUTPUT is deliberately not here: it is the last thing in the chain,
        // after MIX, so it has to scale the blend and not just the wet half.
        return shaped (carrier * params.preGain, lid);
    }

    // One sample of both channels, WTF only. The two channels stop being
    // independent above 50% intensity -- the correction is computed from the
    // pair -- so this cannot be a per-channel call like the one above.
    //
    // `sc` is the summed sidechain, the same number for both channels, which is
    // what the WTF and MONO routings already hand the detector.
    void processWtfPair (float& left, float& right, float sc) noexcept
    {
        // The two half-wave detector signals the split is made of, and beside
        // them the detector MONO would have built from the same sidechain. The
        // mono one costs no extra filter: the sections are linear, so the two
        // halves' envelopes sum to the envelope of the whole.
        float hPos, hNeg, monoMag;

        if (params.filterPost)
        {
            // The split is before the filter here, exactly as it is per-channel:
            // rectifying first and splitting after would hand both sides the
            // same envelope and there would be no pan left -- SPEC 4.5.
            const auto envPos = filters[0].process (std::max (0.0f, sc));
            const auto envNeg = filters[1].process (std::max (0.0f, -sc));

            // Butterworth rings below zero on a rectified signal and the clamp
            // that fixes that -- SPEC 4.3 -- is why the sum has to be taken
            // before it and not after. Clamping the halves and then adding them
            // is not clamping the sum: whenever the ringing puts one half below
            // zero while the other is above, the two disagree, and the mono
            // detector this mode is aiming at is the second one.
            monoMag = std::max (0.0f, envPos + envNeg);

            hPos = std::max (0.0f, envPos);
            hNeg = std::max (0.0f, envNeg);
            detectors[0] = hPos;
            detectors[1] = hNeg;
        }
        else
        {
            // Both filters see the same bipolar sum and return the same number.
            // Running the second one anyway keeps its state in step with the
            // first, so leaving WTF does not resume channel 1 from a filter that
            // stopped updating however long ago the mode was selected.
            const auto filtered = filters[0].process (sc);
            filters[1].process (sc);
            detectors[0] = detectors[1] = filtered;

            hPos = std::max (0.0f, filtered);
            hNeg = std::max (0.0f, -filtered);
            monoMag = std::abs (filtered);
        }

        // splitAmount 1 is the full split, 0 puts the mono detector on both
        // channels -- which is MONO, since the two lids then agree exactly.
        const auto blend = 1.0f - splitAmount;
        const auto lidL = lidFor (splitAmount * hPos + blend * monoMag);
        const auto lidR = lidFor (splitAmount * hNeg + blend * monoMag);
        lids[0] = lidL;
        lids[1] = lidR;

        const auto drivenL = left * params.preGain;
        const auto drivenR = right * params.preGain;

        auto outL = shaped (drivenL, lidL);
        auto outR = shaped (drivenR, lidR);

        // Above 50% the pair is slid until its mid is the mono result. What is
        // left between the channels is untouched, so the difference the split
        // makes -- the whole of the effect -- survives in the sides while the
        // mono sum cancels back to exactly what MONO would have produced. That
        // cancellation is the point: the clipping a mono listener hears is the
        // two channels' opposite halves annihilating, not either channel's own.
        //
        // The correction is the same number on both channels, which is what
        // makes it cancel; a per-channel one would not.
        if (midBlend > 0.0f)
        {
            const auto lidMono = lidFor (monoMag);
            const auto monoL = shaped (drivenL, lidMono);
            const auto monoR = shaped (drivenR, lidMono);

            const auto correction = midBlend * 0.5f * ((monoL + monoR) - (outL + outR));
            outL += correction;
            outR += correction;

            // A width control in the mid/side sense -- the same thing a Utility
            // dropped in after the plugin does, and mono-blind for the same
            // reason: what it scales is added to one channel and subtracted from
            // the other, so the sum never sees it.
            //
            // What it scales is only the side the *effect invented*. The pair's
            // own stereo image is already in monoL/monoR, and subtracting that
            // side out first is what keeps this from being an ordinary widener
            // bolted to the output -- material the lid is not touching comes
            // through untouched however far this is pushed.
            //
            // midBlend is the width too: the side ends up at 1 + midBlend times
            // the invented one, so 100% is the double width SPEC 4.5 stops at.
            const auto invented = 0.5f * ((outL - outR) - (monoL - monoR));
            const auto extra = midBlend * invented;
            outL += extra;
            outR -= extra;
        }

        // Above 50% neither channel is clamped to the lid any more -- one of them
        // is deliberately pushed past it by the half of the excess it was handed.
        // That is unavoidable: two signals both clamped at the lid sum to
        // something also clamped at the lid, so a per-channel ceiling and an
        // exact mono sum cannot both hold. The ceiling becomes a mono-sum
        // ceiling, and the scope's aperture stops bounding the trace it draws.
        left = outL;
        right = outR;
    }

    float lastLid (int channel) const noexcept { return lids[(size_t) channel]; }

    // The signal the CEILING and FLOOR thresholds actually measure -- what the
    // scope draws in cyan. Bipolar in PRE mode, an envelope in POST.
    float lastDetector (int channel) const noexcept { return detectors[(size_t) channel]; }

    const Params& getParams() const noexcept { return params; }

private:
    // t = clamp((mag - floor) / (ceiling - floor), 0, 1), then lid = 1 - t^p.
    float lidFor (float mag) const noexcept
    {
        return shapeTable.lid (std::clamp ((mag - params.floorLin) * windowScale, 0.0f, 1.0f));
    }

    // The lid applied to an already-driven carrier: a wall, or a gain.
    float shaped (float driven, float lid) const noexcept
    {
        return params.clip ? std::clamp (driven, -lid, lid) : driven * lid;
    }

    Params params;
    ShapeTable shapeTable;
    std::vector<DetectorFilter> filters { 2 };
    std::vector<float> lids { 1.0f, 1.0f };
    std::vector<float> detectors { 0.0f, 0.0f };
    double sampleRate = 44100.0;
    float windowScale = 2.0f;

    // WTF intensity, split into the two things it moves. Both are 0 at 50%,
    // which is why that setting is the original WTF exactly. See setParams.
    float splitAmount = 1.0f; // 0 = no split (MONO), 1 = the full split
    float midBlend = 0.0f;    // 1 = mid replaced by MONO's, invented side x2
};

} // namespace hardcap
