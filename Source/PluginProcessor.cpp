#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ids
{
    constexpr auto pre       = "pre";
    constexpr auto ceiling   = "ceiling";
    constexpr auto floorDb   = "floor";
    constexpr auto shape     = "shape";
    constexpr auto filterHz  = "filter";
    constexpr auto slope     = "slope";
    constexpr auto output    = "output";
    constexpr auto clip      = "clip";
    constexpr auto filterPos = "filterpos";
    constexpr auto scLink    = "sclink";
    constexpr auto scSource  = "scsource";
}

// The FILTER sweep tops out at 20 kHz and the top detent is OFF: Nyquist at
// 48 kHz is 24 kHz, so above ~20 kHz the filter is doing nothing anyway.
static constexpr float filterOffHz = 20000.0f;
static constexpr float floorOffDb = -60.0f;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout HardCapProcessor::createParameterLayout()
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
            [] (float v, int)
            {
                if (v >= filterOffHz - 1.0f) return String ("OFF");
                return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz"
                                    : String (roundToInt (v)) + " Hz";
            })));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::slope, 1 }, "Slope",
        StringArray { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct",
                      "30 dB/oct", "36 dB/oct", "42 dB/oct", "48 dB/oct" }, 1));

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

    return layout;
}

//==============================================================================
HardCapProcessor::HardCapProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true))
{
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

    engine.prepare (sampleRate * oversampleFactor, numCh);

    detector.setSize (numCh, maximumExpectedSamplesPerBlock, false, true, false);
    detector.clear();

    using OS = juce::dsp::Oversampling<float>;
    carrierOs = std::make_unique<OS> ((size_t) numCh, oversampleFactorLog2,
                                      OS::filterHalfBandFIREquiripple, true, true);
    detectorOs = std::make_unique<OS> ((size_t) numCh, oversampleFactorLog2,
                                       OS::filterHalfBandFIREquiripple, true, true);

    carrierOs->initProcessing ((size_t) maximumExpectedSamplesPerBlock);
    detectorOs->initProcessing ((size_t) maximumExpectedSamplesPerBlock);
    carrierOs->reset();
    detectorOs->reset();

    // The detector oversampler only ever runs its up path, so its group delay
    // matches the carrier's up path and the two stay sample-aligned.
    setLatencySamples (juce::roundToInt (carrierOs->getLatencyInSamples()));

    pullParameters();
}

void HardCapProcessor::pullParameters()
{
    const auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    hardcap::Params p;
    p.preGain = juce::Decibels::decibelsToGain (raw (ids::pre));
    p.outGain = juce::Decibels::decibelsToGain (raw (ids::output));
    p.ceilingLin = juce::Decibels::decibelsToGain (raw (ids::ceiling));

    const auto floorDb = raw (ids::floorDb);
    p.floorLin = floorDb <= floorOffDb ? 0.0f : juce::Decibels::decibelsToGain (floorDb);

    p.shape = raw (ids::shape);

    const auto hz = raw (ids::filterHz);
    p.filterHz = hz >= filterOffHz - 1.0f ? 0.0f : hz;
    p.poles = (int) raw (ids::slope) + 1;

    p.clip = raw (ids::clip) > 0.5f;
    p.filterPost = raw (ids::filterPos) > 0.5f;

    engine.setParams (p);

    ceilingLinear.store (engine.getParams().ceilingLin, std::memory_order_relaxed);
    floorLinear.store (engine.getParams().floorLin, std::memory_order_relaxed);
}

void HardCapProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, true, 0);
    auto sidechain = getBusBuffer (buffer, true, 1);

    const auto numCh = main.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    if (numCh <= 0 || numSamples <= 0 || carrierOs == nullptr)
        return;

    // Hosts are contractually bound to maximumExpectedSamplesPerBlock, but a few
    // lie. A short block is fine; over-running the detector buffer and the
    // oversamplers is not, so fail safe and pass the block through untouched.
    jassert (numSamples <= detector.getNumSamples());

    if (numSamples > detector.getNumSamples())
        return;

    pullParameters();

    const auto useInternal = apvts.getRawParameterValue (ids::scSource)->load() > 0.5f;
    const auto sumToMono = apvts.getRawParameterValue (ids::scLink)->load() < 0.5f;

    // ---- build the detector source ---------------------------------------
    const auto scChannels = useInternal ? numCh : sidechain.getNumChannels();

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* dst = detector.getWritePointer (ch);

        if (scChannels <= 0)
        {
            juce::FloatVectorOperations::clear (dst, numSamples);
            continue;
        }

        const auto& src = useInternal ? main : sidechain;
        const auto* a = src.getReadPointer (juce::jmin (ch, scChannels - 1));

        if (sumToMono && scChannels > 1)
        {
            const auto* b = src.getReadPointer (1);
            const auto* first = src.getReadPointer (0);

            for (int i = 0; i < numSamples; ++i)
                dst[i] = 0.5f * (first[i] + b[i]);
        }
        else
        {
            juce::FloatVectorOperations::copy (dst, a, numSamples);
        }
    }

    // ---- 8x oversampled section ------------------------------------------
    auto carrierBlock = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) numSamples);
    auto detectorBlock = juce::dsp::AudioBlock<float> (detector)
                             .getSubsetChannelBlock (0, (size_t) numCh)
                             .getSubBlock (0, (size_t) numSamples);

    auto osCarrier = carrierOs->processSamplesUp (carrierBlock);
    auto osDetector = detectorOs->processSamplesUp (detectorBlock);

    const auto osSamples = osCarrier.getNumSamples();

    float* carrierPtr[2] { nullptr, nullptr };
    const float* detectorPtr[2] { nullptr, nullptr };

    for (int ch = 0; ch < numCh && ch < 2; ++ch)
    {
        carrierPtr[ch] = osCarrier.getChannelPointer ((size_t) ch);
        detectorPtr[ch] = osDetector.getChannelPointer ((size_t) ch);
    }

    auto lidMin = 1.0f;
    auto blockLidMin = 1.0f;

    for (size_t i = 0; i < osSamples; ++i)
    {
        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            carrierPtr[ch][i] = engine.processSample (ch, carrierPtr[ch][i], detectorPtr[ch][i]);

        const auto lid = engine.lastLid (0);
        lidMin = juce::jmin (lidMin, lid);
        blockLidMin = juce::jmin (blockLidMin, lid);

        // One scope frame per base-rate sample, taking the most closed lid of
        // the group so the aperture never looks wider than it really was.
        if ((i % (size_t) oversampleFactor) == (size_t) oversampleFactor - 1)
        {
            scope.push ({ engine.lastDetector (0), lidMin, carrierPtr[0][i] });
            lidMin = 1.0f;
        }
    }

    carrierOs->processSamplesDown (carrierBlock);

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
