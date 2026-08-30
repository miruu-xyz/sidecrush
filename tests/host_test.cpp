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

// Choice parameters take a normalised value, and the fraction that lands on a
// given index moves whenever an option is added -- which is exactly how a
// two-way switch grown to three silently redirects every existing call. Name
// the index and let the parameter do the arithmetic.
void setChoice (HardCapProcessor& p, const char* id, int index)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
}

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
    HardCapProcessor p;

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
        HardCapProcessor p;
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
        HardCapProcessor p;
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
            widestLid = juce::jmax (widestLid, std::abs (f.lid - f.lidR));
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

// The cheaper modes are padded so the reported latency never changes. If that
// padding is wrong the host's delay compensation is wrong, which is worse than
// the CPU it saves -- so check where an impulse actually comes out in each mode.
void qualityLatencyIsConstant()
{
    constexpr int blockSize = 512;

    const auto peakOffset = [] (int quality)
    {
        HardCapProcessor p;
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

    HardCapProcessor p;
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

void statePersists()
{
    HardCapProcessor a, b;

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
    HardCapProcessor p;

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

    HardCapProcessor p;
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

    HardCapProcessor p;
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
    HardCapProcessor p;

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
    const auto clamped = hardcap::Engine::clampFloor (1.0f, ceilingLin);
    CHECK (std::abs (juce::Decibels::gainToDecibels (clamped / ceilingLin)
                     - hardcap::Engine::floorHeadroomDb) < 0.01f);
}

// The pills list a parameter's own choices on right-click, and guard on
// getNumSteps so that a continuous parameter -- which would answer with hundreds
// of steps and build a string for every one of them -- is never asked. That
// guard is only as good as the split below.
void onlyChoiceParametersAreListable()
{
    HardCapProcessor p;

    const auto steps = [&p] (const char* id) { return p.apvts.getParameter (id)->getNumSteps(); };

    for (auto* id : { ids::scLink, ids::quality, ids::filterPos, ids::scSource,
                      ids::slope, ids::clip })
        CHECK (steps (id) >= 2 && steps (id) <= 32);

    for (auto* id : { ids::floorDb, ids::ceiling, ids::filterHz, ids::pre,
                      ids::output, ids::mix, ids::shape })
        CHECK (steps (id) > 32);
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
    qualityLatencyIsConstant();
    qualitySwitchDoesNotClick();
    dryPathIsAligned();
    outputTrimsTheBlend();
    floorReadsAsCappedByCeiling();
    onlyChoiceParametersAreListable();
    statePersists();
    rejectsSillyLayouts();

    std::puts ("hardcap host: all checks passed");
    return 0;
}
