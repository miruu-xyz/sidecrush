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

        p.apvts.getParameter ("scsource")->setValueNotifyingHost (internalSource ? 1.0f : 0.0f);
        p.apvts.getParameter ("sclink")->setValueNotifyingHost (monoLink ? 0.0f : 1.0f);

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

// HQ pads the eco path so the reported latency never changes. If that padding is
// wrong the host's delay compensation is wrong, which is worse than the CPU it
// saves -- so check where an impulse actually comes out in both modes.
void hqLatencyIsConstant()
{
    constexpr int blockSize = 512;

    const auto peakOffset = [] (bool hq)
    {
        HardCapProcessor p;
        CHECK (p.setBusesLayout (stereoLayout()));

        p.prepareToPlay (48000.0, blockSize);
        p.apvts.getParameter ("hq")->setValueNotifyingHost (hq ? 1.0f : 0.0f);

        CHECK (p.getLatencySamples() > 0);

        juce::AudioBuffer<float> buffer { 4, blockSize };
        juce::MidiBuffer midi;

        // The swap only lands once the duck has reached silence (~4 ms) and only
        // on a block boundary, and the output is then held down while the pad
        // flushes. Measuring straight away sends the impulse through the OLD
        // path, merely ducked -- which measures HQ twice and never reads
        // ecoPadSamples at all. Feed DC until that has all resolved.
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

    const auto [hqPeak, hqReported] = peakOffset (true);
    const auto [ecoPeak, ecoReported] = peakOffset (false);

    // The headline claim: toggling HQ does not move the reported latency.
    CHECK (hqReported == ecoReported);
    CHECK (ecoPeak > 0);

    // And the padding makes that honest -- the audio really does come out in the
    // same place. Minimum phase is not flat delay, so allow a couple of samples.
    CHECK (std::abs (hqPeak - ecoPeak) <= 2);
    CHECK (std::abs (hqPeak - hqReported) <= 2);
}

// Swapping oversampler cascades steps the output; the duck is what hides it. If
// the duck regresses the plugin clicks whenever anyone touches HQ.
void hqSwitchDoesNotClick()
{
    constexpr int bs = 512;
    constexpr int switchBlock = 20;

    HardCapProcessor p;
    CHECK (p.setBusesLayout (stereoLayout()));

    p.prepareToPlay (48000.0, bs);
    p.apvts.getParameter ("hq")->setValueNotifyingHost (1.0f);

    juce::AudioBuffer<float> buffer { 4, bs };
    juce::MidiBuffer midi;
    std::vector<float> out;
    int n = 0;

    for (int b = 0; b < 40; ++b)
    {
        if (b == switchBlock)
            p.apvts.getParameter ("hq")->setValueNotifyingHost (0.0f);

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
    hqLatencyIsConstant();
    hqSwitchDoesNotClick();
    statePersists();
    rejectsSillyLayouts();

    std::puts ("hardcap host: all checks passed");
    return 0;
}
