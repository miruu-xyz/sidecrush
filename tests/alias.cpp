// How much aliasing does each oversampling factor actually leave?
// Mirrors processBlock's chain but with a fixed, known lid so the result is
// attributable to one nonlinearity at a time.

#include "../Source/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr double sr = 48000.0;
constexpr int fftOrder = 15;
constexpr int N = 1 << fftOrder;      // 32768
int toneBin = 1365;         // 1999.5 Hz -- exact bin, so no window needed

using OS = juce::dsp::Oversampling<float>;

// Mirrors processBlock's topology: carrier at carrierLog2, detector at
// detectorLog2, lid ramped across each group of carrier samples.
std::vector<float> runChain (int carrierLog2, int detectorLog2, OS::FilterType type, bool maxQ,
                             bool clipMode, float carrierAmp, float scAmp, float scDc,
                             OS::FilterType detType = OS::filterHalfBandFIREquiripple,
                             bool detMaxQ = true, int scHarmonics = 1)
{
    const int detectorFactor = 1 << detectorLog2;
    const int blockSize = 512;

    sidecrush::Engine engine;
    engine.prepare (sr * detectorFactor, 1);

    sidecrush::Params p;
    p.ceilingLin = 0.25f;
    p.floorLin = 0.0f;
    p.shape = 0.0f;
    p.filterHz = 0.0f;   // off -- isolate the rectifier, not the filter
    p.clip = clipMode;
    engine.setParams (p);

    std::unique_ptr<OS> carrierOs, detectorOs;

    if (carrierLog2 > 0)
    {
        carrierOs = std::make_unique<OS> (1, (size_t) carrierLog2, type, maxQ, true);
        carrierOs->initProcessing ((size_t) blockSize);
    }

    if (detectorLog2 > 0)
    {
        detectorOs = std::make_unique<OS> (1, (size_t) detectorLog2, detType, detMaxQ, true);
        detectorOs->initProcessing ((size_t) blockSize);
    }

    const int warmup = 8192;
    std::vector<float> out;
    out.reserve ((size_t) (N + warmup));

    juce::AudioBuffer<float> cb { 1, blockSize }, db { 1, blockSize };
    int n = 0;

    while ((int) out.size() < N + warmup)
    {
        for (int i = 0; i < blockSize; ++i, ++n)
        {
            const auto phase = juce::MathConstants<double>::twoPi * toneBin * n / N;
            cb.getWritePointer (0)[i] = carrierAmp * (float) std::sin (phase);
            auto sc = 0.0;

            // A stack of in-phase harmonics is a pulse train: maximal crest
            // factor, so any allpass smear in the detector's upsampler shows up
            // as a change in how far the lid closes.
            for (int h = 1; h <= scHarmonics; ++h)
                sc += std::sin (phase * h) / scHarmonics;

            db.getWritePointer (0)[i] = scDc + scAmp * (float) sc;
        }

        auto cBlock = juce::dsp::AudioBlock<float> (cb);
        auto dBlock = juce::dsp::AudioBlock<float> (db);

        auto uc = carrierOs != nullptr ? carrierOs->processSamplesUp (cBlock) : cBlock;
        auto ud = detectorOs != nullptr ? detectorOs->processSamplesUp (dBlock) : dBlock;

        auto* c = uc.getChannelPointer (0);
        const auto* d = ud.getChannelPointer (0);

        for (size_t i = 0; i < uc.getNumSamples(); ++i)
            c[i] = engine.processSample (0, c[i], d[i]);

        if (carrierOs != nullptr)
            carrierOs->processSamplesDown (cBlock);

        for (int i = 0; i < blockSize; ++i)
            out.push_back (cb.getReadPointer (0)[i]);
    }

    return { out.begin() + warmup, out.begin() + warmup + N };
}

// Energy in bins that are not DC and not a harmonic of the tone, in dB
// relative to the strongest bin.
double aliasFloorDb (const std::vector<float>& signal)
{
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> data (2 * (size_t) N, 0.0f);

    for (int i = 0; i < N; ++i)
        data[(size_t) i] = signal[(size_t) i];

    fft.performFrequencyOnlyForwardTransform (data.data());

    double peak = 0.0, alias = 0.0;

    for (int bin = 1; bin < N / 2; ++bin)
    {
        const auto mag = (double) data[(size_t) bin];
        peak = std::max (peak, mag);

        // Harmonics of the tone (and their immediate skirts) are the wanted
        // distortion; everything else got there by folding.
        const auto isHarmonic = (bin % toneBin) <= 2 || (toneBin - (bin % toneBin)) <= 2;

        if (! isHarmonic)
            alias += mag * mag;
    }

    return 10.0 * std::log10 (alias / (peak * peak) + 1e-30);
}

void row (const char* label, const std::vector<float>& s)
{
    std::printf ("  %-42s %8.1f dB\n", label, aliasFloorDb (s));
}
} // namespace

int main()
{
    using FT = OS::FilterType;
    constexpr auto fir = FT::filterHalfBandFIREquiripple;
    std::printf ("Alias floor relative to the fundamental (more negative = cleaner)\n");
    std::printf ("2 kHz tone, 48 kHz, %d-point FFT\n", N);

    // --- CLIP mode: the hard clipper is the nonlinearity, lid held at 0.3 ---
    std::printf ("\nA. Carrier oversampling -- hard clip, lid held constant at 0.3\n");
    row ("carrier 1x",                  runChain (0, 0, fir, true,  true, 1.0f, 0.0f, 0.175f));
    row ("carrier 2x",                  runChain (1, 1, fir, true,  true, 1.0f, 0.0f, 0.175f));
    row ("carrier 4x",                  runChain (2, 1, fir, true,  true, 1.0f, 0.0f, 0.175f));
    row ("carrier 8x  (shipped)",       runChain (3, 1, fir, true,  true, 1.0f, 0.0f, 0.175f));
    row ("carrier 8x, normal quality",  runChain (3, 1, fir, false, true, 1.0f, 0.0f, 0.175f));
    row ("carrier 8x, IIR polyphase",   runChain (3, 1, FT::filterHalfBandPolyphaseIIR, true, true, 1.0f, 0.0f, 0.175f));

    // --- Detector path: carrier is DC, so the output IS the lid signal ------
    std::printf ("\nB. Detector oversampling -- carrier fixed at 8x, VCA mode, DC carrier,\n");
    std::printf ("   2 kHz into the sidechain, so the output is the lid itself\n");
    row ("detector 1x (no oversampler)", runChain (3, 0, fir, true, false, 1.0f, 0.2f, 0.0f));
    row ("detector 2x  (shipped)",       runChain (3, 1, fir, true, false, 1.0f, 0.2f, 0.0f));
    row ("detector 4x",                  runChain (3, 2, fir, true, false, 1.0f, 0.2f, 0.0f));
    row ("detector 8x  (was)",           runChain (3, 3, fir, true, false, 1.0f, 0.2f, 0.0f));

    std::printf ("\nC. Cheaper detector filters, both paths at 8x (no interpolation)\n");
    row ("detector 8x FIR maxQ",  runChain (3, 3, fir, true, false, 1.0f, 0.2f, 0.0f, fir, true));
    row ("detector 8x FIR normalQ", runChain (3, 3, fir, true, false, 1.0f, 0.2f, 0.0f, fir, false));
    row ("detector 8x IIR polyphase maxQ", runChain (3, 3, fir, true, false, 1.0f, 0.2f, 0.0f, FT::filterHalfBandPolyphaseIIR, true));
    row ("detector 8x IIR polyphase normalQ", runChain (3, 3, fir, true, false, 1.0f, 0.2f, 0.0f, FT::filterHalfBandPolyphaseIIR, false));

    // Swapping the detector's upsampler trades linear phase for minimum phase.
    // The two have different group delays, so compare magnitude spectra (shift
    // invariant) and how far the lid actually closes.
    std::printf ("\nD. Detector upsampler maxQ vs normal Q, 8x both -- does the lid change?\n");

    for (auto harmonics : { 1, 8 })
    {
        const auto a = runChain (3, 3, fir, true, false, 1.0f, 0.6f, 0.0f, fir, true, harmonics);
        const auto b = runChain (3, 3, fir, true, false, 1.0f, 0.6f, 0.0f, fir, false, harmonics);

        juce::dsp::FFT fft { fftOrder };
        std::vector<float> fa (2 * (size_t) N, 0.0f), fb (2 * (size_t) N, 0.0f);
        std::copy (a.begin(), a.end(), fa.begin());
        std::copy (b.begin(), b.end(), fb.begin());
        fft.performFrequencyOnlyForwardTransform (fa.data());
        fft.performFrequencyOnlyForwardTransform (fb.data());

        double num = 0.0, den = 0.0;
        for (int bin = 1; bin < N / 2; ++bin)
        {
            const double d = (double) fa[(size_t) bin] - (double) fb[(size_t) bin];
            num += d * d;
            den += (double) fa[(size_t) bin] * (double) fa[(size_t) bin];
        }

        const auto minA = *std::min_element (a.begin(), a.end());
        const auto minB = *std::min_element (b.begin(), b.end());

        std::printf ("  %d harmonic%-3s spectra differ by %6.1f dB   lid floor %.4f vs %.4f\n",
                     harmonics, harmonics == 1 ? "" : "s",
                     10.0 * std::log10 (num / den + 1e-30), minA, minB);
    }

    // The detector's upsampler is 35 % of the plugin's CPU. Its job is only to
    // feed a rectifier, so a cheaper one is tempting. This asks the only question
    // that matters: does the finished output actually change?
    std::printf ("\nE. Cheaper detector upsampler -- how much does the OUTPUT move?\n");
    std::printf ("   CLIP on, 100 Hz carrier, 8-harmonic sidechain, both paths 8x\n");

    toneBin = (int) std::round (100.0 * N / sr);

    const auto reference = runChain (3, 3, fir, true, true, 1.0f, 0.6f, 0.0f, fir, true, 8);

    const auto compare = [&] (const char* what, const std::vector<float>& other)
    {
        juce::dsp::FFT fft { fftOrder };
        std::vector<float> fa (2 * (size_t) N, 0.0f), fb (2 * (size_t) N, 0.0f);
        std::copy (reference.begin(), reference.end(), fa.begin());
        std::copy (other.begin(), other.end(), fb.begin());
        fft.performFrequencyOnlyForwardTransform (fa.data());
        fft.performFrequencyOnlyForwardTransform (fb.data());

        double num = 0.0, den = 0.0;
        for (int bin = 1; bin < N / 2; ++bin)
        {
            const double d = (double) fa[(size_t) bin] - (double) fb[(size_t) bin];
            num += d * d;
            den += (double) fa[(size_t) bin] * (double) fa[(size_t) bin];
        }

        std::printf ("  %-40s %7.1f dB\n", what, 10.0 * std::log10 (num / den + 1e-30));
    };

    compare ("FIR normal quality", runChain (3, 3, fir, true, true, 1.0f, 0.6f, 0.0f, fir, false, 8));
    compare ("IIR polyphase, max quality",
             runChain (3, 3, fir, true, true, 1.0f, 0.6f, 0.0f, FT::filterHalfBandPolyphaseIIR, true, 8));
    compare ("IIR polyphase, normal quality",
             runChain (3, 3, fir, true, true, 1.0f, 0.6f, 0.0f, FT::filterHalfBandPolyphaseIIR, false, 8));

    return 0;
}
