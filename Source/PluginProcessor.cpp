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

    // CEILING and FLOOR are quoted in dB but the scope plots linear amplitude,
    // and a dB-linear dial puts almost everything in the top of the sweep: on a
    // -60..0 range, half amplitude (-6 dB) sits 90 % of the way round, so the
    // whole lower half of the travel is inaudible fractions. Tapering the dial
    // to amplitude instead makes the pointer and the band it controls move
    // together, which is what "feels linear" means here. -100 rather than the
    // range's own floor is the conversions' minus-infinity, so that the bottom
    // detent round-trips instead of collapsing to zero.
    const auto amplitudeTaper = [] (float minDb)
    {
        return NormalisableRange<float> { minDb, 0.0f,
            [] (float lo, float, float norm)
            {
                return norm <= 0.0f ? lo : jmax (lo, Decibels::gainToDecibels (norm, -100.0f));
            },
            [] (float lo, float, float value)
            {
                return value <= lo ? 0.0f
                                   : jlimit (0.0f, 1.0f, Decibels::decibelsToGain (value, -100.0f));
            },
            [] (float lo, float hi, float value)
            {
                return jlimit (lo, hi, std::round (value * 10.0f) / 10.0f);
            } };
    };

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::pre, 1 }, "Pre",
        NormalisableRange<float> { -36.0f, 36.0f, 0.1f }, 0.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (db)));

    // Held onto because FLOOR's readout has to know where the ceiling is: the two
    // live in the same parameter tree and are destroyed together, so this is the
    // same lifetime the layout itself has.
    auto ceilingParam = std::make_unique<AudioParameterFloat> (
        ParameterID { ids::ceiling, 1 }, "Ceiling",
        amplitudeTaper (-60.0f), -6.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (db));

    const auto* ceiling = ceilingParam.get();
    layout.add (std::move (ceilingParam));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::floorDb, 1 }, "Floor",
        amplitudeTaper (floorOffDb), floorOffDb,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [ceiling] (float v, int)
            {
                // Not "OFF": the floor being at the bottom means the lid starts
                // moving the instant the sidechain does, and reading OFF next to
                // a SHAPE dial invites the reading that the shaping is disabled.
                if (v <= floorOffDb)
                    return String ("INSTANT");

                // The window can never invert, so past CEILING - 0.1 dB the floor
                // stops where the ceiling is and the parameter keeps climbing
                // underneath. Say so rather than reading back a value the audio is
                // not using -- and say it here, in the parameter's own text, so
                // the host's automation lane shows it too.
                if (const auto cap = ceiling->get() + hardcap::Engine::floorHeadroomDb;
                    v > cap)
                    return "- " + String (jmax (floorOffDb, cap), 1) + " dB -";

                return String (v, 1) + " dB";
            })));

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

    // Fully wet by default: this is a limiter first, and a parallel one only if
    // someone asks for it.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::mix, 1 }, "Mix",
        NormalisableRange<float> { 0.0f, 100.0f, 1.0f }, 100.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [] (float v, int) { return String (roundToInt (v)) + "%"; })));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::clip, 1 }, "Clip", true));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::filterPos, 1 }, "Filter Position",
        StringArray { "PRE", "POST" }, 0));

    // WTF sums like MONO and then splits the sum by sign, one half per channel --
    // SPEC 4.5. It is a stereo mode in the sense that the two channels stop
    // agreeing, which is the whole point of it. Last, because the cycle starts
    // at STEREO and the ordinary alternative should be the first click; see the
    // sclink namespace for the rest of why.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::scLink, 1 }, "Sidechain Link",
        StringArray { "STEREO", "MONO", "WTF" }, sclink::stereo));

    // How far WTF is taken. 50% is the original behaviour and the default, so
    // this parameter appearing does not move any session that predates it. 0%
    // hands both channels the whole rectified sum, which is MONO; 100% cancels
    // the effect's leak into the mono sum, so a mono listener hears exactly the
    // MONO result while the stereo image comes apart, and the side the effect
    // invented is widened on the way. See SPEC 4.5.
    //
    // Inert unless SC LINK is on WTF, which is why the editor only shows it
    // there -- but it stays a real parameter in every mode, because a host
    // automating something that vanishes from the list is worse than a host
    // automating something that currently does nothing.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::wtfInt, 1 }, "WTF Intensity",
        NormalisableRange<float> { 0.0f, 100.0f, 1.0f }, 50.0f,
        AudioParameterFloatAttributes{}.withStringFromValueFunction (
            [] (float v, int) { return String (roundToInt (v)) + "%"; })));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::scSource, 1 }, "Sidechain Source",
        StringArray { "EXT", "INT" }, 0));

    // HQ first: nobody should have to find a button to get the good version.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::quality, 1 }, "Quality",
        StringArray { "HQ", "LQ", "YUCK" }, 0));

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
            get (ids::output),    get (ids::mix),       get (ids::clip),
            get (ids::filterPos),
            get (ids::scLink),    get (ids::wtfInt),
            get (ids::scSource),  get (ids::quality) };
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

    const auto build = [&] (int quality)
    {
        // YUCK is factor 0, which JUCE builds as a pass-through stage -- the
        // filter type is then never consulted.
        auto os = std::make_unique<OS> ((size_t) numCh, (size_t) qualityLog2[quality],
                                        quality == 0 ? OS::filterHalfBandFIREquiripple
                                                     : OS::filterHalfBandPolyphaseIIR,
                                        true, true);
        os->initProcessing ((size_t) maximumExpectedSamplesPerBlock);
        os->reset();
        return os;
    };

    for (int q = 0; q < numQualities; ++q)
    {
        carrierOs[(size_t) q] = build (q);
        detectorOs[(size_t) q] = build (q);
    }

    const auto hqLatency = juce::roundToInt (carrierOs[0]->getLatencyInSamples());

    for (int q = 0; q < numQualities; ++q)
        padSamples[(size_t) q] = juce::jlimit (
            0, maxWetLatency,
            hqLatency - juce::roundToInt (carrierOs[(size_t) q]->getLatencyInSamples()));

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maximumExpectedSamplesPerBlock,
                                  (juce::uint32) numCh };
    latencyPad.prepare (spec);
    latencyPad.reset();

    jassert (hqLatency <= maxWetLatency);
    mixer.prepare (spec);
    mixer.setRampLength (juce::Seconds { 0.0 }); // resets, so it has to come first
    mixer.setWetLatency ((float) hqLatency);

    // All of it, not just the gain: a hold left over from a switch that was
    // still in flight when the host re-prepared would duck the first few
    // milliseconds of the new configuration for no reason.
    switchGain = 1.0f;
    switchHold = 0;
    switchRampStep = 1.0f / juce::jmax (1.0f, 0.004f * (float) sampleRate); // ~4 ms
    lastQuality = juce::jlimit (0, numQualities - 1, (int) raw.quality->load());
    engine.prepare (sampleRate * factorFor (lastQuality), numCh);

    // Within any one mode the detector oversampler only ever runs its up path, so
    // its group delay matches the carrier's up path and the two stay aligned.
    // Across modes the cheaper paths are padded, so this figure never moves.
    setLatencySamples (hqLatency);

    pullParameters();
}

void HardCapProcessor::pullParameters()
{
    hardcap::Params p;
    p.preGain = juce::Decibels::decibelsToGain (raw.pre->load());
    p.ceilingLin = juce::Decibels::decibelsToGain (raw.ceiling->load());

    const auto floorDb = raw.floorDb->load();
    p.floorLin = floorDb <= floorOffDb ? 0.0f : juce::Decibels::decibelsToGain (floorDb);

    p.shape = raw.shape->load();

    const auto hz = raw.filterHz->load();
    p.filterHz = hz >= filterOffHz - 1.0f ? 0.0f : hz;
    p.poles = (int) raw.slope->load() + 1;

    p.clip = raw.clip->load() > 0.5f;
    p.filterPost = raw.filterPos->load() > 0.5f;
    p.wtf = (int) raw.scLink->load() == sclink::wtf;
    p.wtfIntensity = raw.wtfInt->load() * 0.01f;

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

    if (numCh <= 0 || numSamples <= 0 || carrierOs[0] == nullptr)
        return;

    // Hosts are contractually bound to maximumExpectedSamplesPerBlock, but a few
    // lie. A short block is fine; over-running the detector buffer and the
    // oversamplers is not, so fail safe and pass the block through untouched.
    jassert (numSamples <= detector.getNumSamples());

    if (numSamples > detector.getNumSamples())
        return;

    pullParameters();

    // Before the oversampler, which writes its result back over this same block.
    mixer.pushDrySamples (juce::dsp::AudioBlock<const float> (main)
                              .getSubBlock (0, (size_t) numSamples));

    // ---- quality ----------------------------------------------------------
    // The swap only happens once the duck has reached silence, and only on a
    // block boundary. Going back before then simply cancels the duck.
    const auto quality = juce::jlimit (0, numQualities - 1, (int) raw.quality->load());

    if (quality != lastQuality && switchGain <= 0.0f)
    {
        lastQuality = quality;
        engine.reset();
        engine.setSampleRate (baseSampleRate * factorFor (quality));

        // The path being switched to has been sitting idle with stale state.
        carrierOs[(size_t) quality]->reset();
        detectorOs[(size_t) quality]->reset();

        latencyPad.reset();
        switchHold = padSamples[(size_t) quality] + 128; // pad flush, plus the cascade filling
    }

    const auto ducking = (quality != lastQuality);
    const auto factor = factorFor (lastQuality);
    auto& carrierOsNow = *carrierOs[(size_t) lastQuality];
    auto& detectorOsNow = *detectorOs[(size_t) lastQuality];

    const auto useInternal = raw.scSource->load() > 0.5f;

    // WTF sums the sidechain exactly as MONO does; the two halves it splits the
    // sum into are the engine's business, one channel each.
    const auto sumToMono = (int) raw.scLink->load() != sclink::stereo;

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

    // ---- oversampled section ----------------------------------------------
    auto carrierBlock = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) numSamples);
    auto osCarrier = carrierOsNow.processSamplesUp (carrierBlock);

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

        auto osDetector = detectorOsNow.processSamplesUp (detectorBlock);

        for (int ch = 0; ch < numCh && ch < 2; ++ch)
            detectorPtr[ch] = osDetector.getChannelPointer ((size_t) (sharedDetector ? 0 : ch));
    }

    auto lidMin = 1.0f;
    auto lidMinR = 1.0f;
    auto blockLidMin = 1.0f;
    const auto channels = juce::jmin (numCh, 2);

    // In WTF the two lids close in turn, so the scope draws one per side. Every
    // other mode drives both from the same detector and the right-hand copy is
    // the left one -- which is also what a mono instance gets.
    const auto stereoLids = channels > 1;

    // Above 50% intensity WTF's two channels are no longer independent -- the
    // correction that makes the mono sum cancel is computed from the pair -- so
    // a stereo instance in WTF takes the engine's two-channel entry point. Both
    // detector pointers are the same summed signal there, so either will do.
    const auto wtfPair = engine.getParams().wtf && stereoLids;

    for (size_t i = 0; i < osSamples; ++i)
    {
        if (wtfPair)
            engine.processWtfPair (carrierPtr[0][i], carrierPtr[1][i], detectorPtr[0][i]);
        else
            for (int ch = 0; ch < channels; ++ch)
                carrierPtr[ch][i] = engine.processSample (ch, carrierPtr[ch][i], detectorPtr[ch][i]);

        const auto lid = engine.lastLid (0);
        const auto lidR = stereoLids ? engine.lastLid (1) : lid;
        lidMin = juce::jmin (lidMin, lid);
        lidMinR = juce::jmin (lidMinR, lidR);
        blockLidMin = juce::jmin (blockLidMin, lid, lidR);

        // One scope frame per base-rate sample, taking the most closed lid of
        // the group so the aperture never looks wider than it really was. `out`
        // is filled in after the downsampler: picking every factor-th sample out
        // of this buffer would fold the clipper's own harmonics into the trace
        // and draw aliasing the real output does not have.
        if ((i % (size_t) factor) == (size_t) factor - 1)
        {
            pendingScope[i / (size_t) factor] = { engine.lastDetector (0), lidMin, 0.0f,
                                                  lidMinR, 0.0f };
            lidMin = 1.0f;
            lidMinR = 1.0f;
        }
    }

    carrierOsNow.processSamplesDown (carrierBlock);

    // Pad the cheaper paths back out to HQ's latency so the reported figure never
    // changes. Fed in every mode, so the buffer is always warm when one flips.
    latencyPad.setDelay ((float) padSamples[(size_t) lastQuality]);

    auto gainAfterBlock = switchGain;
    auto holdAfterBlock = switchHold;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = main.getWritePointer (ch);
        auto gain = switchGain;
        auto hold = switchHold;

        for (int i = 0; i < numSamples; ++i)
        {
            latencyPad.pushSample (ch, d[i]);

            const auto target = (ducking || hold > 0) ? 0.0f : 1.0f;

            if (hold > 0)
                --hold;

            gain = target > gain ? juce::jmin (target, gain + switchRampStep)
                                 : juce::jmax (target, gain - switchRampStep);

            d[i] = latencyPad.popSample (ch) * gain;
        }

        gainAfterBlock = gain;
        holdAfterBlock = hold;
    }

    switchGain = gainAfterBlock;
    switchHold = holdAfterBlock;

    // MIX, then OUTPUT. The duck above is there to hide a splice in the
    // processed path; the dry side has no splice to hide, so it is not ducked.
    // OUTPUT comes last so it scales the blend rather than only the wet half --
    // otherwise pulling MIX down would make the plugin louder by whatever OUT
    // was trimming.
    auto block = juce::dsp::AudioBlock<float> (main).getSubBlock (0, (size_t) numSamples);

    mixer.setWetMixProportion (raw.mix->load() * 0.01f);
    mixer.mixWetSamples (block);

    main.applyGain (0, numSamples, juce::Decibels::decibelsToGain (raw.output->load()));

    // Now the trace really is what the host receives -- downsampled, padded and
    // ducked, all three of which happened above.
    const auto* finished = main.getReadPointer (0);
    const auto* finishedR = main.getReadPointer (numCh > 1 ? 1 : 0);

    for (int i = 0; i < numSamples; ++i)
    {
        pendingScope[(size_t) i].out = finished[i];
        pendingScope[(size_t) i].outR = finishedR[i];
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
