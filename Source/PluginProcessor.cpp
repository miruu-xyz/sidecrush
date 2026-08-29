#include "PluginProcessor.h"
#include "PluginEditor.h"

// The FILTER sweep tops out at 20 kHz and the top detent is OFF: Nyquist at
// 48 kHz is 24 kHz, so above ~20 kHz the filter is doing nothing anyway.

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout HardCapProcessor::createParameterLayout (
    std::atomic<bool>* filterIsPostFlag)
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto db = [] (float v, int) { return String (v, 1) + " dB"; };

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::pre, 1 }, "Pre",
        NormalisableRange<float> { -36.0f, 36.0f, 0.1f }, 0.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (db)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::ceiling, 1 }, "Ceiling",
        NormalisableRange<float> { -60.0f, 0.0f, 0.1f }, -6.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (db)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::floorDb, 1 }, "Floor",
        NormalisableRange<float> { floorOffDb, 0.0f, 0.1f }, floorOffDb,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [] (float v, int) { return v <= floorOffDb ? String ("OFF") : String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::shape, 1 }, "Shape",
        NormalisableRange<float> { -1.0f, 1.0f, 0.01f }, 0.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [] (float v, int) { return String (v, 2); })));

    auto filterRange = NormalisableRange<float> { 20.0f, filterOffHz, 1.0f };
    filterRange.setSkewForCentre (632.0f);
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::filterHz, 1 }, "Filter", filterRange, filterOffHz,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [filterIsPostFlag] (float v, int)
            {
                if (v >= filterOffHz - 1.0f) return String ("OFF");

                // In POST the filter sits after the rectifier, so the knob is an
                // envelope release, not a frequency -- SPEC 4.3 asks the readout
                // to say which. tau = 1 / (2 pi fc): 20 Hz reads as ~8 ms.
                if (filterIsPostFlag->load (std::memory_order_relaxed))
                    return String (1000.0f / (MathConstants<float>::twoPi * v), 2) + " ms";

                return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz"
                                    : String (roundToInt (v)) + " Hz";
            })));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::slope, 1 }, "Slope",
        StringArray { "6dB/oct", "12dB/oct", "18dB/oct", "24dB/oct",
                      "30dB/oct", "36dB/oct", "42dB/oct", "48dB/oct" }, 1));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::output, 1 }, "Output",
        NormalisableRange<float> { -36.0f, 36.0f, 0.1f }, 0.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (db)));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::clip, 1 }, "Clip", true));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::filterPos, 1 }, "Filter Position",
        StringArray { "PRE", "POST" }, 0));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::scLink, 1 }, "Sidechain Link",
        StringArray { "MONO", "STEREO" }, 1));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::scSource, 1 }, "Sidechain Source",
        StringArray { "EXT", "INT" }, 0));

    // On by default: nobody should have to find a button to get the good version.
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::hq, 1 }, "HQ", true));

    return layout;
}

//==============================================================================
HardCapProcessor::HardCapProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true))
{
    const auto get = [this] (const char* id) { return apvts.getRawParameterValue (id); };

    raw = { get (ids::pre),       get (ids::ceiling),   get (ids::floorDb),
            get (ids::shape),     get (ids::filterHz),  get (ids::slope),
            get (ids::output),    get (ids::clip),      get (ids::filterPos),
            get (ids::scLink),    get (ids::scSource),  get (ids::hq) };
}

bool HardCapProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    // SPEC 4.4: mono -> mono and stereo -> stereo only.
    if (mainIn != mainOut)
        return false;

    if (mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;

    const auto sc = layouts.getChannelSet (true, 1);

    return sc.isDisabled()
        || sc == juce::AudioChannelSet::mono()
        || sc == juce::AudioChannelSet::stereo();
}

void HardCapProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    const auto numCh = juce::jmax (1, getMainBusNumInputChannels());
    baseSampleRate = sampleRate;

    detector.setSize (numCh, maximumExpectedSamplesPerBlock, false, true, false);
    detector.clear();

    pendingScope.assign ((size_t) juce::jmax (1, maximumExpectedSamplesPerBlock), ScopeFrame {});

    using OS = juce::dsp::Oversampling<float>;

    const auto build = [&] (int log2Factor, OS::FilterType type)
    {
        auto os = std::make_unique<OS> ((size_t) numCh, log2Factor, type, true, true);
        os->initProcessing ((size_t) maximumExpectedSamplesPerBlock);
        os->reset();
        return os;
    };

    carrierOsHq   = build (hqFactorLog2,  OS::filterHalfBandFIREquiripple);
    detectorOsHq  = build (hqFactorLog2,  OS::filterHalfBandFIREquiripple);
    carrierOsEco  = build (ecoFactorLog2, OS::filterHalfBandPolyphaseIIR);
    detectorOsEco = build (ecoFactorLog2, OS::filterHalfBandPolyphaseIIR);

    const auto hqLatency = juce::roundToInt (carrierOsHq->getLatencyInSamples());
    const auto ecoLatency = juce::roundToInt (carrierOsEco->getLatencyInSamples());
    ecoPadSamples = juce::jlimit (0, 512, hqLatency - ecoLatency);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maximumExpectedSamplesPerBlock,
                                  (juce::uint32) numCh };
    ecoPad.prepare (spec);
    ecoPad.reset();

    // All of it, not just the gain: a hold left over from a switch that was
    // still in flight when the host re-prepared would duck the first few
    // milliseconds of the new configuration for no reason.
    switchGain = 1.0f;
    switchHold = 0;
    switchRampStep = 1.0f / juce::jmax (1.0f, 0.004f * (float) sampleRate); // ~4 ms
    lastHq = raw.hq->load() > 0.5f;
    engine.prepare (sampleRate * (lastHq ? hqFactor : ecoFactor), numCh);

    // Within either mode the detector oversampler only ever runs its up path, so
    // its group delay matches the carrier's up path and the two stay aligned.
    // Across modes the eco path is padded, so this figure never moves.
    setLatencySamples (hqLatency);

    pullParameters();
}

void HardCapProcessor::pullParameters()
{
    hardcap::Params p;
    p.preGain = juce::Decibels::decibelsToGain (raw.pre->load());
    p.outGain = juce::Decibels::decibelsToGain (raw.output->load());
    p.ceilingLin = juce::Decibels::decibelsToGain (raw.ceiling->load());

    const auto floorDb = raw.floorDb->load();
    p.floorLin = floorDb <= floorOffDb ? 0.0f : juce::Decibels::decibelsToGain (floorDb);

    p.shape = raw.shape->load();

    const auto hz = raw.filterHz->load();
    p.filterHz = hz >= filterOffHz - 1.0f ? 0.0f : hz;
    p.poles = (int) raw.slope->load() + 1;

    p.clip = raw.clip->load() > 0.5f;
    p.filterPost = raw.filterPos->load() > 0.5f;

    // Applied every block, no smoothing and no change detection -- SPEC 2. The
    // only part worth guarding is the filter's tan and sins, and DetectorFilter
    // guards that itself.
    engine.setParams (p);

    filterIsPost.store (p.filterPost, std::memory_order_relaxed);
}

void HardCapProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, true, 0);
    auto sidechain = getBusBuffer (buffer, true, 1);

    const auto numCh = main.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    if (numCh <= 0 || numSamples <= 0 || carrierOsHq == nullptr)
        return;

    // Hosts are contractually bound to maximumExpectedSamplesPerBlock, but a few
    // lie. A short block is fine; over-running the detector buffer and the
    // oversamplers is not, so fail safe and pass the block through untouched.
    jassert (numSamples <= detector.getNumSamples());

    if (numSamples > detector.getNumSamples())
        return;

    pullParameters();

    // ---- HQ ---------------------------------------------------------------
    // The swap only happens once the duck has reached silence, and only on a
    // block boundary. Toggling back before then simply cancels the duck.
    const auto hq = raw.hq->load() > 0.5f;

    if (hq != lastHq && switchGain <= 0.0f)
    {
        lastHq = hq;
        engine.reset();
        engine.setSampleRate (baseSampleRate * (hq ? hqFactor : ecoFactor));

        // The path being switched to has been sitting idle with stale state.
        (hq ? *carrierOsHq : *carrierOsEco).reset();
        (hq ? *detectorOsHq : *detectorOsEco).reset();

        ecoPad.reset();
        switchHold = ecoPadSamples + 128; // pad flush, plus the cascade filling
    }

    const auto ducking = (hq != lastHq);
    const auto factor = lastHq ? hqFactor : ecoFactor;
    auto& carrierOs = lastHq ? *carrierOsHq : *carrierOsEco;
    auto& detectorOs = lastHq ? *detectorOsHq : *detectorOsEco;

    const auto useInternal = raw.scSource->load() > 0.5f;
    const auto sumToMono = raw.scLink->load() < 0.5f;

    // ---- build the detector source ---------------------------------------
    const auto scChannels = useInternal ? numCh : sidechain.getNumChannels();

    // The detector's upsampler is about a third of this plugin's CPU, and in
    // several routings it is being asked for something we already have. Both
    // shortcuts below are exact -- tests/host_test.cpp checks them bit for bit
    // against the long way round.
    //
    // A mono sum, a mono sidechain and an unrouted one all put the same signal
    // on every detector channel, so only one of them is worth upsampling.
    const auto sharedDetector = sumToMono || scChannels <= 1;

    // With INT and no mono sum the detector IS the main input, and both
    // oversamplers are the same design -- so the carrier's upsampled block is
    // already the answer. Reading and writing one index in a single step is
    // safe, which is what lets the two share a buffer.
    const auto reuseCarrier = useInternal && scChannels > 0 && ! (sumToMono && scChannels > 1);

    const auto detectorChannels = sharedDetector ? 1 : numCh;

    for (int ch = 0; ! reuseCarrier && ch < detectorChannels; ++ch)
    {
        auto* dst = detector.getWritePointer (ch);

        if (scChannels <= 0)
        {
            juce::FloatVectorOperations::clear (dst, numSamples);
            continue;
        }

        const auto& src = useInternal ? main : sidechain;

        if (sumToMono && scChannels > 1)
        {
            const auto* l = src.getReadPointer (0);
            const auto* r = src.getReadPointer (1);

            for (int i = 0; i < numSamples; ++i)
                dst[i] = 0.5f * (l[i] + r[i]);
        }
        else
        {
            juce::FloatVectorOperations::copy (
                dst, src.getReadPointer (juce::jmin (ch, scChannels - 1)), numSamples);
        }
    }

    // ---- 8x oversampled section ------------------------------------------
    auto carrierBlock = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) numSamples);
    auto osCarrier = carrierOs.processSamplesUp (carrierBlock);

    const auto osSamples = osCarrier.getNumSamples();

    float* carrierPtr[2] { nullptr, nullptr };
    const float* detectorPtr[2] { nullptr, nullptr };

    for (int ch = 0; ch < numCh && ch < 2; ++ch)
        carrierPtr[ch] = osCarrier.getChannelPointer ((size_t) ch);

    if (reuseCarrier)
    {
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            detectorPtr[ch] = carrierPtr[ch];
    }
    else
    {
        auto detectorBlock = juce::dsp::AudioBlock<float> (detector)
                                 .getSubsetChannelBlock (0, (size_t) detectorChannels)
                                 .getSubBlock (0, (size_t) numSamples);

        auto osDetector = detectorOs.processSamplesUp (detectorBlock);

        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            detectorPtr[ch] = osDetector.getChannelPointer ((size_t) (sharedDetector ? 0 : ch));
    }

    auto lidMin = 1.0f;
    auto blockLidMin = 1.0f;
    const auto channels = juce::jmin (numCh, 2);

    for (size_t i = 0; i < osSamples; ++i)
    {
        for (int ch = 0; ch < channels; ++ch)
            carrierPtr[ch][i] = engine.processSample (ch, carrierPtr[ch][i], detectorPtr[ch][i]);

        const auto lid = engine.lastLid (0);
        lidMin = juce::jmin (lidMin, lid);
        blockLidMin = juce::jmin (blockLidMin, lid);

        // One scope frame per base-rate sample, taking the most closed lid of
        // the group so the aperture never looks wider than it really was. `out`
        // is filled in after the downsampler: picking every factor-th sample out
        // of this buffer would fold the clipper's own harmonics into the trace
        // and draw aliasing the real output does not have.
        if ((i % (size_t) factor) == (size_t) factor - 1)
        {
            pendingScope[i / (size_t) factor] = { engine.lastDetector (0), lidMin, 0.0f };
            lidMin = 1.0f;
        }
    }

    carrierOs.processSamplesDown (carrierBlock);

    // Pad eco back out to HQ's latency so the reported figure never changes.
    // Fed in both modes, so the buffer is always warm when the mode flips.
    ecoPad.setDelay ((float) (lastHq ? 0 : ecoPadSamples));

    auto gainAfterBlock = switchGain;
    auto holdAfterBlock = switchHold;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = main.getWritePointer (ch);
        auto gain = switchGain;
        auto hold = switchHold;

        for (int i = 0; i < numSamples; ++i)
        {
            ecoPad.pushSample (ch, d[i]);

            const auto target = (ducking || hold > 0) ? 0.0f : 1.0f;

            if (hold > 0)
                --hold;

            gain = target > gain ? juce::jmin (target, gain + switchRampStep)
                                 : juce::jmax (target, gain - switchRampStep);

            d[i] = ecoPad.popSample (ch) * gain;
        }

        gainAfterBlock = gain;
        holdAfterBlock = hold;
    }

    switchGain = gainAfterBlock;
    switchHold = holdAfterBlock;

    // Now the trace really is what the host receives -- downsampled, padded and
    // ducked, all three of which happened above.
    const auto* finished = main.getReadPointer (0);

    for (int i = 0; i < numSamples; ++i)
    {
        pendingScope[(size_t) i].out = finished[i];
        scope.push (pendingScope[(size_t) i]);
    }

    gainReduction.store (1.0f - blockLidMin, std::memory_order_relaxed);

    // Anything past the main bus (the sidechain's slot in the shared buffer)
    // must not leak to the output.
    for (int ch = getMainBusNumOutputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
}

//==============================================================================
juce::AudioProcessorEditor* HardCapProcessor::createEditor()
{
    return new HardCapEditor (*this);
}

void HardCapProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = apvts.copyState(); auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void HardCapProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HardCapProcessor();
}
