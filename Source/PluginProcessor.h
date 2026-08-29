#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "HardCapEngine.h"

//==============================================================================
struct ScopeFrame
{
    float sc = 0.0f;   // filtered sidechain, the signal the thresholds measure
    float lid = 1.0f;  // 0 = fully shut, 1 = wide open
    float out = 0.0f;
};

// Single producer (audio thread), single consumer (editor timer). The consumer
// only ever reads the most recent frames, so a monotonic write index over a
// power-of-two ring is enough -- no allocation, no locks, no blocking.
//
// ponytail: a torn frame is possible if the editor reads exactly as the audio
// thread overwrites that slot. At 60 fps over a 32k ring that is one bad pixel
// column at worst. Use AbstractFifo if it ever becomes visible.
class ScopeFifo
{
public:
    static constexpr int capacity = 32768;
    static constexpr int mask = capacity - 1;

    void push (ScopeFrame f) noexcept
    {
        const auto w = writePos.load (std::memory_order_relaxed);
        buffer[(size_t) (w & mask)] = f;
        writePos.store (w + 1, std::memory_order_release);
    }

    int64_t head() const noexcept { return writePos.load (std::memory_order_acquire); }

    const ScopeFrame& at (int64_t index) const noexcept
    {
        return buffer[(size_t) (index & mask)];
    }

private:
    std::array<ScopeFrame, (size_t) capacity> buffer {};
    std::atomic<int64_t> writePos { 0 };
};

//==============================================================================
class HardCapProcessor final : public juce::AudioProcessor
{
public:
    HardCapProcessor();
    ~HardCapProcessor() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HardCap"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "HARDCAP", createParameterLayout() };
    ScopeFifo scope;

    // Read by the editor to draw the threshold lines on the same axis as the trace.
    std::atomic<float> ceilingLinear { 0.5f };
    std::atomic<float> floorLinear { 0.0f };
    std::atomic<float> gainReduction { 0.0f }; // 0 = lid open, 1 = fully shut

private:
    // Both paths run at 8x, and tests/alias.cpp says both need to.
    //
    // Carrier: the hard clipper's alias floor is -32 dB at 1x, -48 at 2x, -60 at
    // 4x, -69 at 8x. Every halving costs about 9 dB.
    //
    // Detector: the rectifier's own floor bottoms out sooner, so running it
    // slower and interpolating the lid up to the carrier's rate looks tempting.
    // It is not -- the interpolation's images land on the decimator's fold
    // points, and 2x + interpolation measures -53 dB against 8x's -85 dB. The
    // lid has to be computed at the rate it is used at.
    static constexpr int hqFactorLog2 = 3; // 8x -- SPEC 4.1
    static constexpr int hqFactor = 1 << hqFactorLog2;

    // HQ off: 4x, and minimum phase instead of linear. Measured -60 dB of alias
    // floor against HQ's -69 dB, for about a third of the CPU. The polyphase IIR
    // is both cheaper and slightly cleaner than the FIR at the same factor; what
    // it costs is linear phase, which is a fair thing to spend in an eco mode.
    static constexpr int ecoFactorLog2 = 2; // 4x
    static constexpr int ecoFactor = 1 << ecoFactorLog2;

    void pullParameters();

    hardcap::Engine engine;
    juce::AudioBuffer<float> detector;

    // Both configurations are built up front so that toggling HQ never allocates
    // on the audio thread.
    std::unique_ptr<juce::dsp::Oversampling<float>> carrierOsHq, detectorOsHq,
                                                    carrierOsEco, detectorOsEco;

    // Eco's oversamplers are shorter than HQ's, so the output is padded back out
    // to the same total. Reported latency then never changes and the host is
    // never asked to renegotiate PDC just because someone hit the button.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> ecoPad { 512 };
    int ecoPadSamples = 0;
    bool lastHq = true;
    double baseSampleRate = 44100.0;

    // Linear phase and minimum phase do not line up, so swapping cascades steps
    // the output no matter how carefully the new one is primed. Duck across the
    // change instead: cheaper than running both paths for a block, and a short
    // dip is far less objectionable than a click.
    float switchGain = 1.0f;
    float switchRampStep = 1.0f;

    // Samples to stay silent after the swap. The padding delay line changes tap
    // at the same moment, so for a short while it is still handing back audio the
    // old path produced -- coming back up before that flushes splices the two
    // together, which is the very click the duck exists to hide.
    int switchHold = 0;

    // Last values pushed to the engine, so an unchanged block skips the
    // coefficient maths entirely.
    hardcap::Params lastParams;
    bool haveLastParams = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardCapProcessor)
};
