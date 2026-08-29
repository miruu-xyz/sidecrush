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
    static constexpr int oversampleFactorLog2 = 3; // 8x -- SPEC 4.1
    static constexpr int oversampleFactor = 1 << oversampleFactorLog2;

    void pullParameters();

    hardcap::Engine engine;
    juce::AudioBuffer<float> detector;
    std::unique_ptr<juce::dsp::Oversampling<float>> carrierOs, detectorOs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardCapProcessor)
};
