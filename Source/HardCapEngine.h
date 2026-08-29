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
    float outGain = 1.0f;      // linear
    float ceilingLin = 0.5f;   // linear amplitude
    float floorLin = 0.0f;     // linear amplitude, forced below ceilingLin
    float shape = 0.0f;        // -1 .. +1
    float filterHz = 0.0f;     // <= 0 or >= nyquist*0.45 means off
    int poles = 2;             // 1..8
    bool clip = true;          // true = clip ceiling, false = VCA multiply
    bool filterPost = false;   // rectifier order
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

    // The window can never invert: SPEC 2, floor is clamped to ceiling - 1 dB.
    // The editor draws the same clamp, so it lives here rather than inside
    // setParams -- two copies of this number would drift apart silently, and the
    // symptom would be a floor band that does not sit where the floor is.
    static constexpr float floorHeadroom = 0.891250938f; // -1 dB

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

        for (auto& f : filters)
            f.setup (params.filterHz, params.poles, sampleRate);
    }

    // One sample of one channel. `sc` is the detector input for this channel.
    float processSample (int channel, float carrier, float sc) noexcept
    {
        auto& filter = filters[(size_t) channel];

        // Filter before rectifier keeps the detector at waveform rate. Reversing
        // them turns it into an envelope follower -- see SPEC 4.3.
        // In PRE the filtered signal is still bipolar, which is what the scope
        // draws the symmetric thresholds against. In POST it is already an
        // envelope, and Butterworth ringing can drive it below zero -- clamp so
        // the lid never exceeds 1 and the exponent never sees a negative base.
        float mag;

        if (params.filterPost)
        {
            const auto env = std::max (0.0f, filter.process (std::abs (sc)));
            detectors[(size_t) channel] = env;
            mag = env;
        }
        else
        {
            const auto filtered = filter.process (sc);
            detectors[(size_t) channel] = filtered;
            mag = std::abs (filtered);
        }

        const auto t = std::clamp ((mag - params.floorLin) * windowScale, 0.0f, 1.0f);
        const auto lid = shapeTable.lid (t);
        lids[(size_t) channel] = lid;

        const auto driven = carrier * params.preGain;
        const auto shaped = params.clip ? std::clamp (driven, -lid, lid)
                                        : driven * lid;

        return shaped * params.outGain;
    }

    float lastLid (int channel) const noexcept { return lids[(size_t) channel]; }

    // The signal the CEILING and FLOOR thresholds actually measure -- what the
    // scope draws in cyan. Bipolar in PRE mode, an envelope in POST.
    float lastDetector (int channel) const noexcept { return detectors[(size_t) channel]; }

    const Params& getParams() const noexcept { return params; }

private:
    Params params;
    ShapeTable shapeTable;
    std::vector<DetectorFilter> filters { 2 };
    std::vector<float> lids { 1.0f, 1.0f };
    std::vector<float> detectors { 0.0f, 0.0f };
    double sampleRate = 44100.0;
    float windowScale = 2.0f;
};

} // namespace hardcap
