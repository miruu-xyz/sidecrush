#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "SideCrushEngine.h"

//==============================================================================
// Shared with the editor: a parameter id is a pairing between two files, and a
// constant only keeps them in sync if both halves actually use it.
namespace ids
{
    constexpr auto pre       = "pre";
    constexpr auto ceiling   = "ceiling";
    constexpr auto floorDb   = "floor";
    constexpr auto shape     = "shape";
    constexpr auto filterHz  = "filter";
    constexpr auto slope     = "slope";
    constexpr auto output    = "output";
    constexpr auto mix       = "mix";
    constexpr auto clip      = "clip";
    constexpr auto filterPos = "filterpos";
    constexpr auto scLink    = "sclink";
    constexpr auto wtfInt    = "wtfint";
    constexpr auto scSource  = "scsource";
    constexpr auto quality   = "quality";
}

// The top of the filter's range and the bottom of the floor's both mean "off"
// rather than a value. The editor needs them too -- it blanks the slope
// selector and flattens the filter dial's pointer at that end of the sweep.
constexpr float filterOffHz = 20000.0f;
constexpr float floorOffDb = -60.0f;

// SC LINK is one three-way switch, not two: WTF is a third way of relating the
// detector's two channels, which is what this selector already chooses between.
// The order is the order the pill cycles in -- STEREO, the default, steps first
// to MONO and only then to WTF, so the ordinary linking choice is one click away
// and the strange one is past it. That also leaves the far end open: a second
// WTF engine would be another entry after this one rather than a renumbering of
// everything before it. Both source files index this parameter, and so do the
// tests.
namespace sclink
{
    constexpr int stereo = 0;
    constexpr int mono = 1;
    constexpr int wtf = 2;
}

//==============================================================================
struct ScopeFrame
{
    float sc = 0.0f;    // filtered sidechain, the signal the thresholds measure
    float lid = 1.0f;   // left lid. 0 = fully shut, 1 = wide open
    float out = 0.0f;   // left output
    float lidR = 1.0f;  // the right channel's pair. Identical to the left in
    float outR = 0.0f;  // every mode but WTF, which is the one that draws them
};

// Single producer (audio thread), single consumer (editor timer). The consumer
// only ever reads the most recent frames, so a monotonic write index over a
// power-of-two ring is enough -- no locks, no blocking, and no allocation once
// the ring is built.
//
// The ring is heap-held rather than inline because it is 640 KB and the
// processor owns it by value: two processors as locals is 1.25 MB, which
// overflows the 1 MB stack Windows gives the main thread and segfaults before
// a single check runs. tests/host_test.cpp does exactly that in
// dryPathIsAligned, so this is not hypothetical -- it is what CI hit the moment
// ScopeFrame grew its second channel. Hosts allocate the processor themselves
// and never noticed.
//
// ponytail: a torn frame is possible if the editor reads exactly as the audio
// thread overwrites that slot. At 30 fps over a 32k ring that is one bad pixel
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
    std::vector<ScopeFrame> buffer = std::vector<ScopeFrame> ((size_t) capacity);
    std::atomic<int64_t> writePos { 0 };
};

//==============================================================================
class SideCrushProcessor final : public juce::AudioProcessor
{
public:
    SideCrushProcessor();
    ~SideCrushProcessor() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SideCrush"; }
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

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout (
        std::atomic<bool>* filterIsPostFlag);

    // In POST the FILTER knob is a release time, not a frequency, and the
    // parameter's own text function has to say so -- SPEC 4.3. Declared before
    // apvts on purpose: members are destroyed in reverse, so the parameters
    // holding this pointer are gone before it is.
    std::atomic<bool> filterIsPost { false };

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "SIDECRUSH",
                                               createParameterLayout (&filterIsPost) };
    ScopeFifo scope;

    std::atomic<float> gainReduction { 0.0f }; // 0 = lid open, 1 = fully shut

private:
    // Three quality modes, in the order the parameter lists them. Both paths run
    // at the same factor in all three, and tests/alias.cpp says they need to.
    //
    // Carrier: the hard clipper's alias floor is -32 dB at 1x, -48 at 2x, -60 at
    // 4x, -69 at 8x. Every halving costs about 9 dB.
    //
    // Detector: the rectifier's own floor bottoms out sooner, so running it
    // slower and interpolating the lid up to the carrier's rate looks tempting.
    // It is not -- the interpolation's images land on the decimator's fold
    // points, and 2x + interpolation measures -53 dB against 8x's -85 dB. The
    // lid has to be computed at the rate it is used at.
    //
    // HQ  8x linear phase FIR      -69 dB   -- SPEC 4.1
    // LQ  4x minimum phase IIR     -60 dB, about a third of the CPU. The
    //     polyphase IIR is both cheaper and slightly cleaner than the FIR at the
    //     same factor; what it costs is linear phase, a fair thing to spend here.
    // YUCK 1x, factor 0            -32 dB. JUCE's pass-through stage: not a
    //     cheaper anti-imaging filter but no filter at all, so neither phase
    //     response applies and the aliasing is the effect.
    static constexpr int numQualities = 3;
    static constexpr int qualityLog2[numQualities] { 3, 2, 0 };

    static constexpr int factorFor (int quality) noexcept
    {
        return 1 << qualityLog2[quality];
    }

    void pullParameters();

    // Cached once in the constructor. getRawParameterValue walks a std::map with
    // string comparisons, and these pointers never move after apvts is built.
    struct Raw
    {
        std::atomic<float>* pre, *ceiling, *floorDb, *shape, *filterHz, *slope,
                          *output, *mix, *clip, *filterPos, *scLink, *wtfInt,
                          *scSource, *quality;
    };

    Raw raw {};

    sidecrush::Engine engine;
    juce::AudioBuffer<float> detector;

    // MIX. The dry side goes in before the oversampler writes over the main
    // buffer and comes back delayed by exactly the latency the wet path reports
    // -- otherwise the blend is a comb filter rather than a blend. The ramp is
    // switched off in prepareToPlay: SPEC 2 wants every parameter applied as a
    // block-rate step, and MIX is no exception.
    static constexpr int maxWetLatency = 512;
    juce::dsp::DryWetMixer<float> mixer { maxWetLatency };

    // sc and lid are known inside the oversampled loop, but the output only
    // exists after the downsampler, the pad and the duck -- so the frames are
    // staged here and pushed once all three have run.
    std::vector<ScopeFrame> pendingScope;

    // Every configuration is built up front so that changing quality never
    // allocates on the audio thread.
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, numQualities> carrierOs, detectorOs;

    // The cheaper cascades are shorter than HQ's, so their output is padded back
    // out to the same total. Reported latency then never changes and the host is
    // never asked to renegotiate PDC just because someone hit the button.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> latencyPad { maxWetLatency };
    std::array<int, numQualities> padSamples {};
    int lastQuality = 0;
    double baseSampleRate = 44100.0;

    // No two of the three cascades line up, so swapping one for another steps the
    // output no matter how carefully the new one is primed. Duck across the
    // change instead: cheaper than running both paths for a block, and a short
    // dip is far less objectionable than a click.
    float switchGain = 1.0f;
    float switchRampStep = 1.0f;

    // Samples to stay silent after the swap. The padding delay line changes tap
    // at the same moment, so for a short while it is still handing back audio the
    // old path produced -- coming back up before that flushes splices the two
    // together, which is the very click the duck exists to hide.
    int switchHold = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SideCrushProcessor)
};
