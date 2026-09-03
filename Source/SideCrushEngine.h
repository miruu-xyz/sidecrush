#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

// SideCrush DSP core.
//
// Deliberately free of JUCE types so the whole signal path can be exercised by
// tests/engine_test.cpp without a plugin host. See SPEC.md section 1.

namespace sidecrush
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
    bool recti = false;        // clip only the half the sidechain's polarity points at
    float rectiBlend = 1.0f;   // 0 = that rectified result, 1 = the symmetric one
};

class Engine
{
public:
    void prepare (double newSampleRate, int numChannels)
    {
        sampleRate = newSampleRate;

        // Two filters per channel, not one: RECTI and WTF both need the
        // sidechain's two half-waves followed separately, and in POST the
        // rectifier has already thrown the sign away by the time the filter
        // sees it -- so the split has to happen upstream of a filter each.
        // Index 2*ch carries the positive half (and the whole bipolar signal in
        // PRE, where one filter still does), 2*ch+1 the negative half.
        const auto channels = (size_t) std::max (1, numChannels);
        filters.resize (2 * channels);
        lidsTop.assign (channels, 1.0f);
        lidsBot.assign (channels, 1.0f);
        detectors.assign (channels, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : filters)
            f.reset();

        std::fill (lidsTop.begin(), lidsTop.end(), 1.0f);
        std::fill (lidsBot.begin(), lidsBot.end(), 1.0f);
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

    // The detector, taken apart into everything downstream needs from it. The
    // two halves are what RECTI clips against and what WTF pans with; `mag` is
    // the symmetric magnitude every mode before them used.
    struct Detect
    {
        float mag;   // what a single lid measures
        float pos;   // the positive half of the sidechain
        float neg;   // the negative half
        float trace; // what the scope draws: bipolar in PRE, an envelope in POST
    };

    // One channel's detector, filtered. Costs two filter sections' worth of
    // state in POST and one in PRE, because PRE's filter is still looking at a
    // bipolar signal and the split can happen after it -- SPEC 4.3.
    Detect detect (int channel, float sc) noexcept
    {
        auto& fPos = filters[(size_t) (2 * channel)];
        auto& fNeg = filters[(size_t) (2 * channel + 1)];

        if (params.filterPost)
        {
            // The split comes before the filter: the rectifier is the thing that
            // destroys the sign, so anything downstream that needs the polarity
            // has to be handed two signals here or it never gets one. This is
            // the same order processWtfPair has always used.
            const auto envPos = fPos.process (std::max (0.0f, sc));
            const auto envNeg = fNeg.process (std::max (0.0f, -sc));

            // Summed before the clamp, not after: Butterworth rings below zero
            // on a rectified signal, and clamping the halves first would make
            // this disagree with the envelope of the whole whenever one half is
            // ringing negative while the other is not. envPos + envNeg is
            // filter(|sc|) exactly -- the sections are linear -- so a mode that
            // is not using the split gets the same number it always did.
            const auto mag = std::max (0.0f, envPos + envNeg);

            return { mag, std::max (0.0f, envPos), std::max (0.0f, envNeg), mag };
        }

        // PRE filters the bipolar sidechain, so the sign survives it and the
        // split is free. The trace stays bipolar so the scope draws the whole
        // wave: in WTF its top lobe is the left channel and its bottom lobe the
        // right, and in RECTI each lobe is the half of the carrier it clips.
        const auto filtered = fPos.process (sc);

        return { std::abs (filtered), std::max (0.0f, filtered),
                 std::max (0.0f, -filtered), filtered };
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
        const auto d = detect (channel, sc);

        // WTF hands both channels the same summed sidechain and gives each one
        // half of it: the left is driven by its positive excursions, the right by
        // its negative ones -- SPEC 4.5.
        const auto half = channel == 0 ? d.pos : d.neg;

        // What one lid would measure, and the two RECTI splits it into. Outside
        // WTF that is simply the sidechain's own two halves, so the carrier is
        // clipped on the side the sidechain is currently pointing at and left
        // alone on the other. Inside it, the channel only ever sees its own
        // half, so its far side has nothing to close it and stays wide open.
        const auto symMag = params.wtf ? half : d.mag;
        const auto topMag = params.wtf ? (channel == 0 ? half : 0.0f) : d.pos;
        const auto botMag = params.wtf ? (channel == 0 ? 0.0f : half) : d.neg;

        detectors[(size_t) channel] = params.wtf && params.filterPost ? symMag : d.trace;

        const auto sym = lidFor (symMag);
        setLids (channel, sym, lidFor (topMag), lidFor (botMag));

        // OUTPUT is deliberately not here: it is the last thing in the chain,
        // after MIX, so it has to scale the blend and not just the wet half.
        return shapedRecti (carrier * params.preGain, sym,
                            lidsTop[(size_t) channel], lidsBot[(size_t) channel]);
    }

    // One sample of both channels, WTF only. The two channels stop being
    // independent above 50% intensity -- the correction is computed from the
    // pair -- so this cannot be a per-channel call like the one above.
    //
    // `sc` is the summed sidechain, the same number for both channels, which is
    // what the WTF and MONO routings already hand the detector.
    void processWtfPair (float& left, float& right, float sc) noexcept
    {
        // Both channels' filter pairs see the same summed sidechain, so they
        // return the same numbers and either will do. Running the second one
        // anyway keeps its state in step with the first, so leaving WTF does not
        // resume channel 1 from a filter that stopped updating however long ago
        // the mode was selected.
        const auto d = detect (0, sc);
        detect (1, sc);

        // The two half-wave detector signals the split is made of, and beside
        // them the detector MONO would have built from the same sidechain. The
        // mono one costs no extra filter: the sections are linear, so the two
        // halves' envelopes sum to the envelope of the whole -- see detect().
        const auto hPos = d.pos;
        const auto hNeg = d.neg;
        const auto monoMag = d.mag;

        detectors[0] = params.filterPost ? hPos : d.trace;
        detectors[1] = params.filterPost ? hNeg : d.trace;

        // splitAmount 1 is the full split, 0 puts the mono detector on both
        // channels -- which is MONO, since the two lids then agree exactly.
        const auto blend = 1.0f - splitAmount;
        const auto lidL = lidFor (splitAmount * hPos + blend * monoMag);
        const auto lidR = lidFor (splitAmount * hNeg + blend * monoMag);

        // RECTI's pair, per channel. Each channel keeps the lid belonging to its
        // own half of the sidechain at every intensity -- that half is what WTF
        // handed it -- and the far side is closed only by however much of the
        // split has not been taken yet. At 0% both channels hold both lids, which
        // is the ordinary rectified result on a mono sum; at 100% each holds one
        // and the other stands wide open.
        setLids (0, lidL, lidFor (hPos), lidFor (blend * hNeg));
        setLids (1, lidR, lidFor (blend * hPos), lidFor (hNeg));

        const auto drivenL = left * params.preGain;
        const auto drivenR = right * params.preGain;

        auto outL = shapedRecti (drivenL, lidL, lidsTop[0], lidsBot[0]);
        auto outR = shapedRecti (drivenR, lidR, lidsTop[1], lidsBot[1]);

        // Above 50% the pair is slid until its mid is the mono result. What is
        // left between the channels is untouched, so the difference the split
        // makes -- the whole of the effect -- survives in the sides while the
        // mono sum cancels back to exactly what MONO would have produced. That
        // cancellation is the point: the clipping a mono listener hears is the
        // two channels' opposite halves annihilating, not either channel's own.
        //
        // The correction is the same number on both channels, which is what
        // makes it cancel; a per-channel one would not. It holds whatever shape
        // the carrier went through, RECTI's asymmetric one included, because it
        // is defined as the difference between two sums and not as a lid.
        if (midBlend > 0.0f)
        {
            const auto lidMono = lidFor (monoMag);
            const auto monoTop = lidFor (hPos);
            const auto monoBot = lidFor (hNeg);
            const auto monoL = shapedRecti (drivenL, lidMono, monoTop, monoBot);
            const auto monoR = shapedRecti (drivenR, lidMono, monoTop, monoBot);

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

    // The tighter of the two lids -- what a single symmetric aperture would be,
    // and what the gain-reduction meter has always shown. Outside RECTI the two
    // are the same number.
    float lastLid (int channel) const noexcept
    {
        return std::min (lidsTop[(size_t) channel], lidsBot[(size_t) channel]);
    }

    // The aperture as the scope has to draw it: two edges that move apart under
    // RECTI, because only one of them is clipping anything at a time.
    float lastLidTop (int channel) const noexcept { return lidsTop[(size_t) channel]; }
    float lastLidBot (int channel) const noexcept { return lidsBot[(size_t) channel]; }

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

    // Outside RECTI the two lids are one number, and storing it twice is what
    // keeps every reader -- the meter, the scope, the shaper -- on one path.
    void setLids (int channel, float sym, float top, float bot) noexcept
    {
        lidsTop[(size_t) channel] = params.recti ? top : sym;
        lidsBot[(size_t) channel] = params.recti ? bot : sym;
    }

    // The lid applied to an already-driven carrier: a wall, or a gain.
    float shaped (float driven, float lid) const noexcept
    {
        return params.clip ? std::clamp (driven, -lid, lid) : driven * lid;
    }

    // RECTI. The two lids are independent, so only the half of the waveform the
    // sidechain's own polarity points at is touched and the other passes at full
    // amplitude -- which means RECTI is not a limiter: the ceiling stops holding
    // in one direction, the same admission SPEC 4.5 already makes about WTF above
    // 50%. What it buys is an asymmetry modulated at the sidechain's own rate,
    // which prints the sidechain's period onto the carrier as even harmonics.
    //
    // Under CLIP the asymmetry is a pair of walls at different heights. Without
    // it, it is a pair of gains -- still continuous through zero, since both
    // sides meet at 0, so what comes out is a kink and not a step.
    //
    // rectiBlend crossfades this against the symmetric result. It is the upper
    // half of the MIX fader's travel; the lower half is dry against this, and
    // the mixer upstream does that one.
    float shapedRecti (float driven, float sym, float top, float bot) const noexcept
    {
        if (! params.recti)
            return shaped (driven, sym);

        const auto rect = params.clip ? std::clamp (driven, -bot, top)
                                      : driven * (driven > 0.0f ? top : bot);

        return rect + params.rectiBlend * (shaped (driven, sym) - rect);
    }

    Params params;
    ShapeTable shapeTable;
    std::vector<DetectorFilter> filters { 4 };
    std::vector<float> lidsTop { 1.0f, 1.0f };
    std::vector<float> lidsBot { 1.0f, 1.0f };
    std::vector<float> detectors { 0.0f, 0.0f };
    double sampleRate = 44100.0;
    float windowScale = 2.0f;

    // WTF intensity, split into the two things it moves. Both are 0 at 50%,
    // which is why that setting is the original WTF exactly. See setParams.
    float splitAmount = 1.0f; // 0 = no split (MONO), 1 = the full split
    float midBlend = 0.0f;    // 1 = mid replaced by MONO's, invented side x2
};

} // namespace sidecrush
