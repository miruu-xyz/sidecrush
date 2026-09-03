// Integration smoke test: instantiates the real AudioProcessor and pushes audio
// through it the way a host would. Catches the things the engine test cannot --
// bus layouts, oversampling block sizes, detector buffer sizing, and any NaN
// escaping into the output.

#include "../Source/PluginProcessor.h"
#include "check.h"

#include <cstdio>
#include <utility>
#include <random>

namespace
{

// Stereo in, stereo sidechain, stereo out -- what most of these tests want.
juce::AudioProcessor::BusesLayout stereoLayout()
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    return layout;
}

bool allFinite (const juce::AudioBuffer<float>& b, int numChannels, int numSamples)
{
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (b.getReadPointer (ch)[i]))
                return false;

    return true;
}

void runLayout (const juce::AudioChannelSet& main, const juce::AudioChannelSet& sidechain,
                double sampleRate, int blockSize)
{
    SideCrushProcessor p;

    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (main);
    layout.inputBuses.add (sidechain);
    layout.outputBuses.add (main);

    const auto accepted = p.setBusesLayout (layout);
    CHECK (accepted);

    p.prepareToPlay (sampleRate, blockSize);
    CHECK (p.getLatencySamples() > 0); // 8x FIR oversampling is not free

    const auto totalChannels = main.size() + sidechain.size();
    juce::AudioBuffer<float> buffer (juce::jmax (1, totalChannels), blockSize);
    juce::MidiBuffer midi;

    std::mt19937 rng { 1234 };
    std::uniform_real_distribution<float> noise { -1.0f, 1.0f };
    double phase = 0.0;

    for (int block = 0; block < 200; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            // Carrier: noise, so any flattening is obvious. Sidechain: a 50 Hz
            // sine that comfortably crosses the default -6 dBFS ceiling.
            const auto sub = (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 50.0 / sampleRate;

            for (int ch = 0; ch < main.size(); ++ch)
                buffer.getWritePointer (ch)[i] = noise (rng) * 0.5f;

            for (int ch = 0; ch < sidechain.size(); ++ch)
                buffer.getWritePointer (main.size() + ch)[i] = sub * 0.9f;
        }

        p.processBlock (buffer, midi);
        CHECK (allFinite (buffer, main.size(), blockSize));
    }

    // The sidechain peaks above the ceiling, so the lid must actually be closing.
    CHECK (p.gainReduction.load() > 0.5f);

    // And the scope must be receiving frames at base rate.
    CHECK (p.scope.head() >= (int64_t) blockSize * 200);

    p.releaseResources();
}

// processBlock has two shortcuts that skip the detector oversampler when its
// output would duplicate something already computed: INT + STEREO reuses the
// carrier's upsampled block, and MONO link upsamples one channel for both. Both
// claim to be exact, so check them against a route that takes neither shortcut.
//
// Same signal into main and sidechain: EXT reads the sidechain the long way,
// INT takes the shortcut. The two must agree bit for bit.
void shortcutsAreExact()
{
    constexpr int blockSize = 256;
    constexpr int blocks = 40;

    const auto render = [] (bool internalSource, bool monoLink, bool preSummed)
    {
        SideCrushProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, blockSize);

        setChoice (p, "scsource", internalSource ? 1 : 0);
        setChoice (p, "sclink", monoLink ? sclink::mono : sclink::stereo);

        juce::AudioBuffer<float> buffer { 4, blockSize };
        juce::MidiBuffer midi;
        std::mt19937 rng { 7 };
        std::uniform_real_distribution<float> noise { -1.0f, 1.0f };
        std::vector<float> out;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto l = noise (rng) * 0.8f;
                const auto r = noise (rng) * 0.8f;

                buffer.getWritePointer (0)[i] = l;
                buffer.getWritePointer (1)[i] = r;

                // preSummed hands both sidechain channels the mono sum already,
                // so STEREO link reproduces what MONO link would build.
                buffer.getWritePointer (2)[i] = preSummed ? 0.5f * (l + r) : l;
                buffer.getWritePointer (3)[i] = preSummed ? 0.5f * (l + r) : r;
            }

            p.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    out.push_back (buffer.getReadPointer (ch)[i]);
        }

        return out;
    };

    // INT + STEREO (reuses the carrier block) vs EXT + STEREO fed the same signal.
    const auto viaExt = render (false, false, false);
    const auto viaInt = render (true, false, false);
    CHECK (viaExt.size() == viaInt.size());

    for (size_t i = 0; i < viaExt.size(); ++i)
        CHECK (juce::exactlyEqual (viaExt[i], viaInt[i]));

    // INT + MONO takes the shared-upsample path but not the reuse path.
    const auto viaExtSummed = render (false, true, false);
    const auto viaIntSummed = render (true, true, false);
    CHECK (viaExtSummed.size() == viaIntSummed.size());

    for (size_t i = 0; i < viaExtSummed.size(); ++i)
        CHECK (juce::exactlyEqual (viaExtSummed[i], viaIntSummed[i]));

    // MONO link (one shared upsample) vs STEREO link fed the pre-summed signal.
    const auto viaStereoLink = render (false, false, true);
    const auto viaMonoLink = render (false, true, false);
    CHECK (viaStereoLink.size() == viaMonoLink.size());

    for (size_t i = 0; i < viaStereoLink.size(); ++i)
        CHECK (juce::exactlyEqual (viaStereoLink[i], viaMonoLink[i]));
}

// WTF only pans if the processor sums the sidechain the way MONO does *and*
// tells the engine to split it -- two separate lines reading one parameter
// index. Drive a sub through it and check the two outputs stop agreeing.
void wtfPansTheCarrier()
{
    constexpr int bs = 512;

    // The carrier gap the ears get, and the lid gap the scope draws from -- one
    // run, because the second is only interesting where the first is happening.
    struct Gaps { float carrier, lid; };

    const auto widestChannelGap = [] (int link)
    {
        SideCrushProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, bs);
        setChoice (p, "sclink", link);
        p.apvts.getParameter ("ceiling")->setValueNotifyingHost (0.5f);

        juce::AudioBuffer<float> buffer { 4, bs };
        juce::MidiBuffer midi;
        auto widest = 0.0f;
        int n = 0;

        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < bs; ++i, ++n)
            {
                // A 40 Hz sub on both sidechain channels, white-ish carrier.
                const auto sub = 0.9f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                          * 40.0 * n / 48000.0);
                for (int ch = 0; ch < 2; ++ch)
                    buffer.getWritePointer (ch)[i] = 0.8f;

                buffer.getWritePointer (2)[i] = sub;
                buffer.getWritePointer (3)[i] = sub;
            }

            p.processBlock (buffer, midi);

            if (b < 4) // let the oversamplers fill before measuring
                continue;

            for (int i = 0; i < bs; ++i)
                widest = juce::jmax (widest, std::abs (buffer.getReadPointer (0)[i]
                                                       - buffer.getReadPointer (1)[i]));
        }

        // The editor draws one lid per side in WTF -- the top of the aperture is
        // the left, the bottom the right -- so the frames have to carry both.
        auto widestLid = 0.0f;

        for (auto i = juce::jmax<int64_t> (0, p.scope.head() - bs); i < p.scope.head(); ++i)
        {
            const auto& f = p.scope.at (i);
            widestLid = juce::jmax (widestLid, std::abs (f.lidTop - f.lidTopR));
        }

        return Gaps { widest, widestLid };
    };

    // STEREO fed identical channels has nothing to tell them apart.
    const auto stereo = widestChannelGap (sclink::stereo);
    CHECK (stereo.carrier < 1.0e-5f);
    CHECK (juce::exactlyEqual (stereo.lid, 0.0f));

    // WTF does: at the sub's peaks one lid is shut and the other wide open.
    const auto wtf = widestChannelGap (sclink::wtf);
    CHECK (wtf.carrier > 0.5f);
    CHECK (wtf.lid > 0.5f);
}

// WTF at 100% is a claim about what a *mono listener* hears: the two channels
// come apart, and their sum is what the MONO link would have produced. The
// engine's own test checks the arithmetic; this checks the plugin actually
// reaches it -- two parameters have to be read and one branch taken in
// processBlock for any of it to happen, and the engine test cannot see any of
// that. Summing after the downsampler is legitimate because every stage between
// here and the engine is linear.
void wtfFullIntensitySumsToMono()
{
    constexpr int bs = 512;

    const auto run = [] (int link, float intensity)
    {
        SideCrushProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, bs);
        setChoice (p, "sclink", link);

        auto& wtfInt = *p.apvts.getParameter ("wtfint");
        wtfInt.setValueNotifyingHost (wtfInt.convertTo0to1 (intensity));
        p.apvts.getParameter ("ceiling")->setValueNotifyingHost (0.5f);

        juce::AudioBuffer<float> buffer { 4, bs };
        juce::MidiBuffer midi;
        std::vector<float> sum;
        int n = 0;

        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < bs; ++i, ++n)
            {
                const auto at = [n] (double hz)
                { return (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * n / 48000.0); };

                // Genuinely different carriers: a mistake that corrected the two
                // channels by the same signal rather than the same *number*
                // would still sum correctly if they were identical.
                buffer.getWritePointer (0)[i] = 0.9f * at (700.0);
                buffer.getWritePointer (1)[i] = 0.4f * at (1100.0);

                const auto sub = 0.9f * at (40.0);
                buffer.getWritePointer (2)[i] = sub;
                buffer.getWritePointer (3)[i] = sub;
            }

            p.processBlock (buffer, midi);

            if (b < 4) // let the oversamplers fill before measuring
                continue;

            for (int i = 0; i < bs; ++i)
                sum.push_back (buffer.getReadPointer (0)[i] + buffer.getReadPointer (1)[i]);
        }

        return sum;
    };

    const auto mono = run (sclink::mono, 50.0f); // intensity is inert here
    const auto full = run (sclink::wtf, 100.0f);
    const auto half = run (sclink::wtf, 50.0f);

    CHECK (mono.size() == full.size() && mono.size() == half.size());

    auto worstFull = 0.0f;
    auto worstHalf = 0.0f;

    for (size_t i = 0; i < mono.size(); ++i)
    {
        worstFull = juce::jmax (worstFull, std::abs (full[i] - mono[i]));
        worstHalf = juce::jmax (worstHalf, std::abs (half[i] - mono[i]));
    }

    // 100% sums to MONO. Not "close to": the only arithmetic between the engine
    // and here is a linear resampler both runs share.
    CHECK (worstFull < 1.0e-4f);

    // And the check has teeth: at 50% the same sum is nothing like MONO, which
    // is the leak into mono that 100% exists to remove.
    CHECK (worstHalf > 0.1f);
}

// The cheaper modes are padded so the reported latency never changes. If that
// padding is wrong the host's delay compensation is wrong, which is worse than
// the CPU it saves -- so check where an impulse actually comes out in each mode.
void qualityLatencyIsConstant()
{
    constexpr int blockSize = 512;

    const auto peakOffset = [] (int quality)
    {
        SideCrushProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, blockSize);
        setChoice (p, "quality", quality);

        CHECK (p.getLatencySamples() > 0);

        juce::AudioBuffer<float> buffer { 4, blockSize };
        juce::MidiBuffer midi;

        // The swap only lands once the duck has reached silence (~4 ms) and only
        // on a block boundary, and the output is then held down while the pad
        // flushes. Measuring straight away sends the impulse through the OLD
        // path, merely ducked -- which measures HQ three times and never reads
        // padSamples at all. Feed DC until that has all resolved.
        for (int b = 0; b < 8; ++b)
        {
            buffer.clear();

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25f, blockSize);

            p.processBlock (buffer, midi);
        }

        // The sidechain is silent, so the lid is wide open and DC passes at unity.
        // While the duck is still active this would be 0 -- so it is the assertion
        // that the requested path really is the one live below.
        CHECK (std::abs (buffer.getReadPointer (0)[blockSize - 1] - 0.25f) < 0.01f);

        // Let the DC step's own tail clear before the impulse goes in.
        for (int b = 0; b < 2; ++b) { buffer.clear(); p.processBlock (buffer, midi); }

        auto best = 0.0f;
        auto bestIndex = 0;

        for (int b = 0; b < 4; ++b)
        {
            buffer.clear();

            if (b == 0)
                for (int ch = 0; ch < 2; ++ch)
                    buffer.setSample (ch, 0, 0.5f);

            p.processBlock (buffer, midi);

            for (int i = 0; i < blockSize; ++i)
            {
                const auto v = std::abs (buffer.getReadPointer (0)[i]);

                if (v > best) { best = v; bestIndex = b * blockSize + i; }
            }
        }

        CHECK (best > 0.05f); // the impulse actually came out somewhere

        return std::pair { bestIndex, p.getLatencySamples() };
    };

    const auto [hqPeak, hqReported] = peakOffset (0);

    for (int quality = 1; quality <= 2; ++quality)
    {
        const auto [peak, reported] = peakOffset (quality);

        // The headline claim: changing quality does not move the reported latency.
        CHECK (hqReported == reported);
        CHECK (peak > 0);

        // And the padding makes that honest -- the audio really does come out in
        // the same place. Minimum phase is not flat delay, and YUCK has no
        // anti-imaging filter at all, so allow a couple of samples either way.
        CHECK (std::abs (hqPeak - peak) <= 2);
    }

    CHECK (std::abs (hqPeak - hqReported) <= 2);
}

// Swapping oversampler cascades steps the output; the duck is what hides it. If
// the duck regresses the plugin clicks whenever anyone touches QUALITY. YUCK is
// the furthest jump from HQ, so it is the one worth checking.
void qualitySwitchDoesNotClick()
{
    constexpr int bs = 512;
    constexpr int switchBlock = 20;

    SideCrushProcessor p;
    CHECK (p.setBusesLayout (stereoLayout()));

    p.prepareToPlay (48000.0, bs);
    setChoice (p, "quality", 0);

    juce::AudioBuffer<float> buffer { 4, bs };
    juce::MidiBuffer midi;
    std::vector<float> out;
    int n = 0;

    for (int b = 0; b < 40; ++b)
    {
        if (b == switchBlock)
            setChoice (p, "quality", 2); // the furthest jump: 8x linear phase to none

        for (int i = 0; i < bs; ++i, ++n)
        {
            const auto v = 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                    * 200.0 * n / 48000.0);
            for (int ch = 0; ch < 4; ++ch)
                buffer.getWritePointer (ch)[i] = v;
        }

        p.processBlock (buffer, midi);

        for (int i = 0; i < bs; ++i)
            out.push_back (buffer.getReadPointer (0)[i]);
    }

    const auto worstStep = [&] (int from, int to)
    {
        auto worst = 0.0f;

        for (int i = juce::jmax (1, from); i < to; ++i)
            worst = juce::jmax (worst, std::abs (out[(size_t) i] - out[(size_t) (i - 1)]));

        return worst;
    };

    const auto steady = worstStep (bs * 5, bs * (switchBlock - 1));
    const auto seam = worstStep (bs * switchBlock - 8, bs * switchBlock + 1200);

    CHECK (steady > 0.0f);

    // Unducked this lands around 22x. Anything near the steady-state step means
    // the seam is buried in the waveform's own movement.
    CHECK (seam < steady * 2.0f);
}

// The scope's mask is a reading of the *output*, so it has to follow MIX: below
// full wet the dry half it is blended with was never clipped, and a mask still
// clamped to the lid would be drawing a signal the plug-in did not produce.
void apertureFollowsMix()
{
    constexpr int bs = 512;

    const auto tightestEdge = [] (bool recti, float mixPercent)
    {
        SideCrushProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, bs);
        p.apvts.getParameter ("ceiling")->setValueNotifyingHost (0.35f);
        setChoice (p, "recti", recti ? 1 : 0);
        p.apvts.getParameter ("mix")->setValueNotifyingHost (mixPercent / 100.0f);

        juce::AudioBuffer<float> buffer { 4, bs };
        juce::MidiBuffer midi;
        int n = 0;

        for (int b = 0; b < 12; ++b)
        {
            for (int i = 0; i < bs; ++i, ++n)
            {
                const auto sub = 0.9f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                          * 40.0 * n / 48000.0);
                for (int ch = 0; ch < 2; ++ch)
                    buffer.getWritePointer (ch)[i] = 0.8f;

                buffer.getWritePointer (2)[i] = sub;
                buffer.getWritePointer (3)[i] = sub;
            }

            p.processBlock (buffer, midi);
        }

        auto tightest = 1.0f;

        for (auto i = juce::jmax<int64_t> (0, p.scope.head() - bs); i < p.scope.head(); ++i)
        {
            const auto& f = p.scope.at (i);
            tightest = juce::jmin (tightest, f.lidTop, f.lidBot);
        }

        return tightest;
    };

    for (const auto recti : { false, true })
    {
        // Fully wet, the sub is well past the ceiling and the aperture shuts.
        CHECK (tightestEdge (recti, 100.0f) < 0.2f);

        // Fully dry, nothing the plug-in does reaches the output, so there is no
        // aperture to draw at all.
        CHECK (juce::exactlyEqual (tightestEdge (recti, 0.0f), 1.0f));

        // And it opens monotonically on the way between the two.
        CHECK (tightestEdge (recti, 25.0f) > tightestEdge (recti, 100.0f));
    }

    // RCTF's midpoint is its fully-processed position, so the mask is as tight
    // there as the symmetric mode's is at the top of the travel.
    CHECK (tightestEdge (true, 50.0f) < 0.2f);
}

void statePersists()
{
    SideCrushProcessor a, b;

    a.apvts.getParameter ("ceiling")->setValueNotifyingHost (0.25f);
    a.apvts.getParameter ("clip")->setValueNotifyingHost (0.0f);

    juce::MemoryBlock state;
    a.getStateInformation (state);
    b.setStateInformation (state.getData(), (int) state.getSize());

    CHECK (juce::approximatelyEqual (a.apvts.getRawParameterValue ("ceiling")->load(),
                                      b.apvts.getRawParameterValue ("ceiling")->load()));
    CHECK (juce::approximatelyEqual (a.apvts.getRawParameterValue ("clip")->load(),
                                      b.apvts.getRawParameterValue ("clip")->load()));
}

void rejectsSillyLayouts()
{
    SideCrushProcessor p;

    juce::AudioProcessor::BusesLayout monoInStereoOut;
    monoInStereoOut.inputBuses.add (juce::AudioChannelSet::mono());
    monoInStereoOut.inputBuses.add (juce::AudioChannelSet::stereo());
    monoInStereoOut.outputBuses.add (juce::AudioChannelSet::stereo());

    CHECK (! p.checkBusesLayoutSupported (monoInStereoOut));
}

// MIX at 0 has to hand back the input untouched and in time. Anything that
// forgets the reported latency shows up here as an impulse in the wrong place.
void dryPathIsAligned()
{
    constexpr int blockSize = 512;

    SideCrushProcessor p;
    CHECK (p.setBusesLayout (stereoLayout()));
    p.prepareToPlay (48000.0, blockSize);

    p.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f);

    // Enough drive and a low enough ceiling that a leaking wet path could not be
    // mistaken for the dry one.
    p.apvts.getParameter ("pre")->setValueNotifyingHost (1.0f);

    const auto latency = p.getLatencySamples();
    CHECK (latency > 0 && latency < blockSize);

    juce::AudioBuffer<float> buffer { 4, blockSize };
    juce::MidiBuffer midi;

    buffer.clear();
    buffer.setSample (0, 0, 0.5f);
    p.processBlock (buffer, midi);

    const auto* out = buffer.getReadPointer (0);
    CHECK (std::abs (out[latency] - 0.5f) < 1.0e-6f);

    for (int i = 0; i < blockSize; ++i)
        if (i != latency)
            CHECK (std::abs (out[i]) < 1.0e-6f);
}

// OUTPUT is the last thing in the chain, after MIX, so it has to scale the
// blend and not just the wet half -- otherwise pulling MIX down makes the
// plugin louder by however much OUT is trimming.
void outputTrimsTheBlend()
{
    constexpr int blockSize = 512;

    SideCrushProcessor p;
    CHECK (p.setBusesLayout (stereoLayout()));
    p.prepareToPlay (48000.0, blockSize);

    p.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f);

    auto& output = *p.apvts.getParameter ("output");
    output.setValueNotifyingHost (output.convertTo0to1 (-12.0f));

    const auto latency = p.getLatencySamples();

    juce::AudioBuffer<float> buffer { 4, blockSize };
    juce::MidiBuffer midi;

    buffer.clear();
    buffer.setSample (0, 0, 0.5f);
    p.processBlock (buffer, midi);

    const auto expected = 0.5f * juce::Decibels::decibelsToGain (-12.0f);
    CHECK (std::abs (buffer.getSample (0, latency) - expected) < 1.0e-6f);
}

// FLOOR can be pushed right up under CEILING, and once the clamp bites the
// parameter keeps climbing while the audio does not -- so the readout has to say
// which value is actually in force. It is the parameter's own text function, so
// the host sees the same string the pill does.
void floorReadsAsCappedByCeiling()
{
    SideCrushProcessor p;

    auto& ceiling = *p.apvts.getParameter (ids::ceiling);
    auto& floor = *p.apvts.getParameter (ids::floorDb);

    const auto setDb = [] (juce::RangedAudioParameter& param, float db)
    {
        param.setValueNotifyingHost (param.convertTo0to1 (db));
    };

    setDb (ceiling, -6.0f);

    // Below the cap: the floor reads as itself.
    setDb (floor, -12.0f);
    CHECK (floor.getCurrentValueAsText() == "-12.0 dB");

    // Above it: bracketed, and reading the value the engine is really using --
    // one 0.1 dB step under the ceiling, which is as close as the two can get.
    setDb (floor, -3.0f);
    CHECK (floor.getCurrentValueAsText() == "- -6.1 dB -");

    // And it follows the ceiling rather than being decided once.
    setDb (ceiling, -24.0f);
    CHECK (floor.getCurrentValueAsText() == "- -24.1 dB -");

    setDb (ceiling, -1.0f);
    CHECK (floor.getCurrentValueAsText() == "-3.0 dB");

    setDb (floor, floorOffDb);
    CHECK (floor.getCurrentValueAsText() == "INSTANT");

    // The clamp the readout is describing: a window one step wide, not a dead one.
    const auto ceilingLin = juce::Decibels::decibelsToGain (-6.0f);
    const auto clamped = sidecrush::Engine::clampFloor (1.0f, ceilingLin);
    CHECK (std::abs (juce::Decibels::gainToDecibels (clamped / ceilingLin)
                     - sidecrush::Engine::floorHeadroomDb) < 0.01f);
}

} // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI init;

    runLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), 48000.0, 512);
    runLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), 44100.0, 64);
    runLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), 96000.0, 1024);
    runLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono(), 48000.0, 256);
    runLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo(), 48000.0, 128);

    shortcutsAreExact();
    wtfPansTheCarrier();
    apertureFollowsMix();
    wtfFullIntensitySumsToMono();
    qualityLatencyIsConstant();
    qualitySwitchDoesNotClick();
    dryPathIsAligned();
    outputTrimsTheBlend();
    floorReadsAsCappedByCeiling();
    statePersists();
    rejectsSillyLayouts();

    std::puts ("sidecrush host: all checks passed");
    return 0;
}
