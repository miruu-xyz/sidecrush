// Throughput benchmark: where does HardCap's CPU actually go?
// Not a test -- not registered with CTest. Run it by hand.

#include "../Source/PluginProcessor.h"
#include "check.h" // for setChoice; this file makes no assertions of its own

#include <chrono>
#include <cstdio>
#include <random>

namespace
{
constexpr double sr = 48000.0;
int blockSize = 512;
int blocks = 4000;

using Clock = std::chrono::steady_clock;

double secondsOf (int n) { return (double) n * (double) blockSize / sr; }

void fillNoise (juce::AudioBuffer<float>& b)
{
    std::mt19937 rng { 1234 };
    std::uniform_real_distribution<float> d { -0.5f, 0.5f };

    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.getWritePointer (ch)[i] = d (rng);
}

// % of one core needed to keep up with real time.
void report (const char* name, double elapsed, int n)
{
    std::printf ("  %-46s %7.2f %% CPU\n", name, 100.0 * elapsed / secondsOf (n));
}

void benchOversampler (const char* name, int log2Factor,
                       juce::dsp::Oversampling<float>::FilterType type,
                       bool maxQuality, bool downToo)
{
    juce::dsp::Oversampling<float> os { 2, (size_t) log2Factor, type, maxQuality, true };
    os.initProcessing ((size_t) blockSize);

    juce::AudioBuffer<float> buf { 2, blockSize };
    fillNoise (buf);

    const auto start = Clock::now();

    for (int b = 0; b < blocks; ++b)
    {
        auto block = juce::dsp::AudioBlock<float> (buf);
        auto up = os.processSamplesUp (block);
        juce::ignoreUnused (up);

        if (downToo)
            os.processSamplesDown (block);
    }

    const std::chrono::duration<double> elapsed = Clock::now() - start;
    report (name, elapsed.count(), blocks);
    std::printf ("  %-46s latency %.1f samples\n", "", os.getLatencyInSamples());
}

void benchEngine (const char* name, int factor)
{
    hardcap::Engine engine;
    engine.prepare (sr * factor, 2);

    hardcap::Params p;
    p.filterHz = 120.0f;
    p.poles = 2;
    engine.setParams (p);

    const int n = blockSize * factor;
    std::vector<float> carrier ((size_t) n), sc ((size_t) n);
    std::mt19937 rng { 99 };
    std::uniform_real_distribution<float> d { -0.5f, 0.5f };

    for (int i = 0; i < n; ++i) { carrier[(size_t) i] = d (rng); sc[(size_t) i] = d (rng); }

    const auto start = Clock::now();
    float sink = 0.0f;

    for (int b = 0; b < blocks; ++b)
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                sink += engine.processSample (ch, carrier[(size_t) i], sc[(size_t) i]);

    const std::chrono::duration<double> elapsed = Clock::now() - start;
    report (name, elapsed.count(), blocks);
    juce::ignoreUnused (sink);
}

// Changing QUALITY swaps one oversampler cascade for another. How audible is
// the seam? HQ to YUCK is the widest of the three jumps.
void measureQualitySwitchGlitch()
{
    constexpr int bs = 512;
    constexpr int totalBlocks = 40;
    constexpr int switchBlock = 20;

    HardCapProcessor proc;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    proc.setBusesLayout (layout);
    proc.prepareToPlay (48000.0, bs);
    setChoice (proc, "quality", 0);

    juce::AudioBuffer<float> buf { 4, bs };
    juce::MidiBuffer midi;
    std::vector<float> out;
    int n = 0;

    for (int b = 0; b < totalBlocks; ++b)
    {
        if (b == switchBlock)
            setChoice (proc, "quality", 2);

        for (int i = 0; i < bs; ++i, ++n)
        {
            const auto v = 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * n / sr);
            for (int ch = 0; ch < 4; ++ch) buf.getWritePointer (ch)[i] = v;
        }

        proc.processBlock (buf, midi);

        for (int i = 0; i < bs; ++i)
            out.push_back (buf.getReadPointer (0)[i]);
    }

    // Biggest sample-to-sample step well away from the seam, versus at it.
    const auto step = [&] (int from, int to)
    {
        auto worst = 0.0f;
        for (int i = juce::jmax (1, from); i < to; ++i)
            worst = juce::jmax (worst, std::abs (out[(size_t) i] - out[(size_t) (i - 1)]));
        return worst;
    };

    const auto steady = step (bs * 5, bs * (switchBlock - 1));
    const auto seam = step (bs * switchBlock - 8, bs * switchBlock + 1600);

    std::printf ("  steady-state step %.5f, worst step across the QUALITY switch %.5f (%.1fx)\n",
                 steady, seam, seam / juce::jmax (1.0e-9f, steady));
}

void benchWholePlugin (double rate = sr, bool internalSource = false, bool monoLink = false,
                       const char* note = nullptr, int quality = 0)
{
    HardCapProcessor proc;

    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    proc.setBusesLayout (layout);

    proc.prepareToPlay (rate, blockSize);

    setChoice (proc, "scsource", internalSource ? 1 : 0);
    setChoice (proc, "sclink", monoLink ? sclink::mono : sclink::stereo);
    setChoice (proc, "quality", quality);

    juce::AudioBuffer<float> buf { 4, blockSize }; // main stereo + sidechain stereo
    fillNoise (buf);
    juce::MidiBuffer midi;

    const auto start = Clock::now();

    for (int b = 0; b < blocks; ++b)
    {
        // Re-noise cheaply: the clipper would otherwise decay to silence.
        buf.applyGain (1.0f);
        proc.processBlock (buf, midi);
    }

    const std::chrono::duration<double> elapsed = Clock::now() - start;
    char label[80];

    if (note != nullptr)
        std::snprintf (label, sizeof (label), "%s", note);
    else
        std::snprintf (label, sizeof (label), "%g kHz, %d-sample blocks", rate / 1000.0, blockSize);
    std::printf ("  %-46s %7.2f %% CPU\n", label,
                 100.0 * elapsed.count() / ((double) blocks * (double) blockSize / rate));
}
} // namespace

int main()
{
    std::printf ("HardCap bench -- 48 kHz, %d-sample blocks, stereo\n", blockSize);
    std::printf ("(%% of one core to run in real time; lower is better)\n\n");

    std::printf ("Whole plugin, one stereo instance:\n");

    for (auto bs : { 64, 128, 256, 512, 1024 })
    {
        blockSize = bs;
        blocks = (int) (48000.0 * 40.0 / bs); // ~40 s of audio either way
        benchWholePlugin();
    }

    blockSize = 512;
    blocks = 4000;
    benchWholePlugin (96000.0);

    std::printf ("\nQuality (48 kHz, 512-sample blocks, EXT + STEREO):\n");
    benchWholePlugin (sr, false, false, "HQ   -- 8x linear phase FIR", 0);
    benchWholePlugin (sr, false, false, "LQ   -- 4x minimum phase IIR", 1);
    benchWholePlugin (sr, false, false, "YUCK -- 1x, no filter at all", 2);
    benchWholePlugin (sr, true, false, "LQ + INT + STEREO", 1);

    std::printf ("\nQuality switch continuity:\n");
    measureQualitySwitchGlitch();

    std::printf ("\nSidechain routing (48 kHz, 512-sample blocks):\n");
    benchWholePlugin (sr, false, false, "EXT + STEREO  (no shortcut)");
    benchWholePlugin (sr, false, true,  "EXT + MONO    (one shared upsample)");
    benchWholePlugin (sr, true,  false, "INT + STEREO  (reuses the carrier block)");
    benchWholePlugin (sr, true,  true,  "INT + MONO    (one shared upsample)");

    std::printf ("\nOversamplers (up only, as the detector uses it):\n");
    using OS = juce::dsp::Oversampling<float>;
    benchOversampler ("8x FIR equiripple, maxQ  (current)", 3, OS::filterHalfBandFIREquiripple, true, false);
    benchOversampler ("4x FIR equiripple, maxQ", 2, OS::filterHalfBandFIREquiripple, true, false);
    benchOversampler ("2x FIR equiripple, maxQ", 1, OS::filterHalfBandFIREquiripple, true, false);
    benchOversampler ("8x IIR polyphase, maxQ", 3, OS::filterHalfBandPolyphaseIIR, true, false);
    benchOversampler ("4x IIR polyphase, maxQ", 2, OS::filterHalfBandPolyphaseIIR, true, false);
    benchOversampler ("2x IIR polyphase, maxQ", 1, OS::filterHalfBandPolyphaseIIR, true, false);
    benchOversampler ("8x FIR equiripple, normal Q", 3, OS::filterHalfBandFIREquiripple, false, false);

    std::printf ("\nOversamplers (up + down, as the carrier uses it):\n");
    benchOversampler ("8x FIR equiripple, maxQ  (current)", 3, OS::filterHalfBandFIREquiripple, true, true);
    benchOversampler ("8x FIR equiripple, normal Q", 3, OS::filterHalfBandFIREquiripple, false, true);
    benchOversampler ("4x FIR equiripple, maxQ", 2, OS::filterHalfBandFIREquiripple, true, true);
    benchOversampler ("2x FIR equiripple, maxQ", 1, OS::filterHalfBandFIREquiripple, true, true);
    benchOversampler ("8x IIR polyphase, maxQ", 3, OS::filterHalfBandPolyphaseIIR, true, true);
    benchOversampler ("4x IIR polyphase, maxQ", 2, OS::filterHalfBandPolyphaseIIR, true, true);

    std::printf ("\nEngine inner loop only (no oversampling filters):\n");
    benchEngine ("engine at 8x", 8);
    benchEngine ("engine at 4x", 4);
    benchEngine ("engine at 1x", 1);

    return 0;
}
