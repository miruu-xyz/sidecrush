// Integration smoke test: instantiates the real AudioProcessor and pushes audio
// through it the way a host would. Catches the things the engine test cannot --
// bus layouts, oversampling block sizes, detector buffer sizing, and any NaN
// escaping into the output.

#include "../Source/PluginProcessor.h"

#include <cstdio>
#include <random>
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

namespace
{

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

    statePersists();
    rejectsSillyLayouts();

    std::puts ("hardcap host: all checks passed");
    return 0;
}
