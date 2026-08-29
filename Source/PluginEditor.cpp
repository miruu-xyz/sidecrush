#include "PluginEditor.h"

//==============================================================================
HardCapLookAndFeel::HardCapLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, hccolour::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, hccolour::faint);
    setColour (juce::ComboBox::textColourId, hccolour::text);
    setColour (juce::ComboBox::backgroundColourId, hccolour::panel);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, juce::Colours::transparentBlack);
    setColour (juce::PopupMenu::backgroundColourId, hccolour::panel);
    setColour (juce::PopupMenu::textColourId, hccolour::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, hccolour::accent.withAlpha (0.2f));
    setColour (juce::PopupMenu::highlightedTextColourId, hccolour::text);
}

void HardCapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                           float pos, float startAngle, float endAngle,
                                           juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = startAngle + pos * (endAngle - startAngle);
    const auto enabled = slider.isEnabled();

    juce::ColourGradient body { hccolour::panel.brighter (0.10f), centre.translated (0.0f, -radius),
                                hccolour::background.darker (0.25f), centre.translated (0.0f, radius), false };
    g.setGradientFill (body);
    g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

    g.setColour (hccolour::line);
    g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    const auto inner = radius * 0.34f;
    const auto outer = radius * 0.94f;
    const juce::Point<float> from { centre.x + inner * std::sin (angle), centre.y - inner * std::cos (angle) };
    const juce::Point<float> to { centre.x + outer * std::sin (angle), centre.y - outer * std::cos (angle) };

    g.setColour (enabled ? hccolour::accent : hccolour::faint);
    g.drawLine ({ from, to }, juce::jmax (2.0f, radius * 0.035f));
}

void HardCapLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                           float pos, float, float,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style == juce::Slider::LinearBar)
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        g.setColour (hccolour::panel);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (hccolour::line);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (slider.isEnabled() ? hccolour::text : hccolour::faint);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (slider.getTextFromValue (slider.getValue()), r, juce::Justification::centred);
        return;
    }

    const auto track = juce::Rectangle<float> ((float) x + (float) w * 0.5f - 0.5f, (float) y, 1.0f, (float) h);
    g.setColour (hccolour::line);
    g.fillRect (track);

    const auto thumbH = 11.0f;
    const auto thumbW = juce::jmin (42.0f, (float) w);
    const auto cy = juce::jlimit ((float) y + thumbH * 0.5f, (float) y + (float) h - thumbH * 0.5f, pos);

    juce::Rectangle<float> thumb { (float) x + ((float) w - thumbW) * 0.5f, cy - thumbH * 0.5f, thumbW, thumbH };
    g.setColour (slider.isEnabled() ? hccolour::dim : hccolour::faint);
    g.fillRoundedRectangle (thumb, 3.0f);
}

void HardCapLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool,
                                       int, int, int, int, juce::ComboBox& box)
{
    juce::Rectangle<float> r { 0.0f, 0.0f, (float) w, (float) h };
    g.setColour (box.isEnabled() ? hccolour::panel : hccolour::panel.withAlpha (0.4f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (hccolour::line);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
}

void HardCapLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (box.getLocalBounds());
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.0f));
    label.setColour (juce::Label::textColourId,
                     box.isEnabled() ? hccolour::text : hccolour::faint);
}

void HardCapLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour&, bool highlighted, bool)
{
    const auto on = button.getToggleState();
    const auto colour = on ? hccolour::floorLine : hccolour::line;
    juce::Rectangle<float> r = button.getLocalBounds().toFloat();

    g.setColour (on ? hccolour::floorLine.withAlpha (0.14f) : hccolour::panel);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (highlighted ? colour.brighter (0.3f) : colour);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

    button.setColour (juce::TextButton::textColourOnId, hccolour::floorLine);
    button.setColour (juce::TextButton::textColourOffId, hccolour::dim);
}

//==============================================================================
ScopeComponent::ScopeComponent (HardCapProcessor& p) : processor (p)
{
    startTimerHz (60);
}

void ScopeComponent::timerCallback()
{
    snapshotHead = processor.scope.head();
    repaint();
}

void ScopeComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (hccolour::background.darker (0.35f));
    g.fillRoundedRectangle (bounds, 12.0f);
    g.setColour (hccolour::line);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);

    const auto& fifo = processor.scope;
    const auto head = snapshotHead;

    if (head < 512)
        return;

    // ---- trigger: most recent rising zero crossing of the sidechain -------
    const auto maxScan = (int64_t) juce::jmin (ScopeFifo::capacity - 8, 12000);
    int64_t trigger = head - 1;
    int64_t previous = -1;
    int found = 0;

    for (int64_t i = head - 2; i > head - maxScan && i > 1; --i)
    {
        if (fifo.at (i - 1).sc <= 0.0f && fifo.at (i).sc > 0.0f)
        {
            if (found == 0) trigger = i;
            else { previous = i; break; }

            ++found;
        }
    }

    // Auto timebase: two cycles of whatever the sidechain is doing, so a 40 Hz
    // sub and a 100 Hz sub fill the window the same way.
    int64_t window = 1024;

    if (previous > 0 && trigger > previous)
        window = juce::jlimit<int64_t> (128, 8192, (trigger - previous) * 2);

    const auto startIndex = juce::jmax<int64_t> (1, trigger - window);
    const auto count = trigger - startIndex;

    if (count < 8)
        return;

    const auto mid = bounds.getCentreY();
    const auto amp = bounds.getHeight() * 0.5f - 10.0f;
    const auto toX = [&] (int64_t i) { return bounds.getX() + (float) (i - startIndex) / (float) count * bounds.getWidth(); };
    const auto toY = [&] (float v) { return mid - juce::jlimit (-1.2f, 1.2f, v) * amp; };

    // ---- lid aperture ----------------------------------------------------
    juce::Path top, bottom;
    top.startNewSubPath (bounds.getX(), bounds.getY());
    bottom.startNewSubPath (bounds.getX(), bounds.getBottom());

    for (int64_t i = startIndex; i < trigger; ++i)
    {
        const auto lid = fifo.at (i).lid;
        top.lineTo (toX (i), toY (lid));
        bottom.lineTo (toX (i), toY (-lid));
    }

    top.lineTo (bounds.getRight(), bounds.getY());
    top.closeSubPath();
    bottom.lineTo (bounds.getRight(), bounds.getBottom());
    bottom.closeSubPath();

    g.setColour (hccolour::dim.withAlpha (0.18f));
    g.fillPath (top);
    g.fillPath (bottom);

    // ---- threshold lines -------------------------------------------------
    const auto ceilingLin = processor.ceilingLinear.load (std::memory_order_relaxed);
    const auto floorLin = processor.floorLinear.load (std::memory_order_relaxed);

    const float dashes[] { 3.0f, 3.0f };

    g.setColour (hccolour::ceiling.withAlpha (0.75f));
    for (auto sign : { 1.0f, -1.0f })
        g.drawLine (bounds.getX(), toY (sign * ceilingLin), bounds.getRight(), toY (sign * ceilingLin), 1.0f);

    g.setColour (hccolour::floorLine.withAlpha (0.75f));
    for (auto sign : { 1.0f, -1.0f })
        g.drawDashedLine ({ bounds.getX(), toY (sign * floorLin),
                            bounds.getRight(), toY (sign * floorLin) }, dashes, 2, 1.0f);

    // ---- traces ----------------------------------------------------------
    juce::Path sidechainPath, outputPath;

    for (int64_t i = startIndex; i < trigger; ++i)
    {
        const auto& f = fifo.at (i);
        const auto x = toX (i);

        if (i == startIndex)
        {
            sidechainPath.startNewSubPath (x, toY (f.sc));
            outputPath.startNewSubPath (x, toY (f.out));
        }
        else
        {
            sidechainPath.lineTo (x, toY (f.sc));
            outputPath.lineTo (x, toY (f.out));
        }
    }

    g.setColour (hccolour::accent);
    g.strokePath (sidechainPath, juce::PathStrokeType (1.6f));

    g.setColour (hccolour::text);
    g.strokePath (outputPath, juce::PathStrokeType (1.4f));

    g.setColour (hccolour::faint);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("HARDCAP", bounds.reduced (12.0f, 8.0f), juce::Justification::bottomLeft);
}

//==============================================================================
HardCapEditor::ActivityLed::ActivityLed (HardCapProcessor& p) : processor (p)
{
    startTimerHz (30);
}

void HardCapEditor::ActivityLed::timerCallback()
{
    const auto target = processor.gainReduction.load (std::memory_order_relaxed);
    level = juce::jmax (target, level * 0.72f); // fast attack, visible decay
    repaint();
}

void HardCapEditor::ActivityLed::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (hccolour::accent.withAlpha (0.12f + 0.88f * juce::jlimit (0.0f, 1.0f, level)));
    g.fillEllipse (r);
}

//==============================================================================
HardCapEditor::HardCapEditor (HardCapProcessor& p)
    : AudioProcessorEditor (&p), proc (p), scope (p), led (p)
{
    setLookAndFeel (&lookAndFeel);

    addSlider (juce::Slider::LinearVertical, "pre", preAtt);
    addSlider (juce::Slider::LinearVertical, "output", outputAtt);
    addSlider (juce::Slider::RotaryVerticalDrag, "ceiling", ceilingAtt);
    addSlider (juce::Slider::RotaryVerticalDrag, "filter", filterAtt);
    addSlider (juce::Slider::RotaryVerticalDrag, "shape", shapeAtt);
    addSlider (juce::Slider::LinearBar, "floor", floorAtt);

    addCombo ("slope", slopeAtt);
    addCombo ("filterpos", filterPosAtt);
    addCombo ("sclink", scLinkAtt);
    addCombo ("scsource", scSourceAtt);

    clipButton.setClickingTogglesState (true);
    addAndMakeVisible (clipButton);
    clipAtt = std::make_unique<ButtonAttachment> (proc.apvts, "clip", clipButton);

    addAndMakeVisible (scope);
    addAndMakeVisible (led);

    setSize (880, 340);
}

HardCapEditor::~HardCapEditor()
{
    setLookAndFeel (nullptr);
}

void HardCapEditor::paint (juce::Graphics& g)
{
    g.fillAll (hccolour::background);

    g.setColour (hccolour::faint);
    g.setFont (juce::FontOptions (14.0f));

    const auto label = [&] (const juce::String& t, juce::Rectangle<int> r)
    {
        g.drawText (t, r, juce::Justification::centred);
    };

    label ("PRE", { 20, 34, 60, 18 });
    label ("CEILING", { 96, 34, 170, 18 });
    label ("OUTPUT", { 366, 34, 70, 18 });

    g.setFont (juce::FontOptions (11.0f));
    label ("FILTER", { 278, 122, 92, 14 });
    label ("SHAPE", { 278, 208, 92, 14 });
}

void HardCapEditor::resized()
{
    preSlider.setBounds (30, 60, 40, 190);
    ceilingKnob.setBounds (101, 56, 160, 160);
    led.setBounds (255, 200, 7, 7);

    slopeBox.setBounds (278, 34, 92, 21);
    filterKnob.setBounds (289, 62, 70, 70);
    shapeKnob.setBounds (289, 148, 70, 70);
    floorField.setBounds (278, 226, 92, 21);

    outputSlider.setBounds (381, 60, 40, 190);

    scope.setBounds (442, 34, 412, 213);

    auto row = juce::Rectangle<int> (442, 256, 412, 21);
    clipButton.setBounds (row.removeFromLeft (52));
    row.removeFromLeft (8);
    filterPosBox.setBounds (row.removeFromLeft (112));
    row.removeFromLeft (8);
    scLinkBox.setBounds (row.removeFromLeft (80));
    row.removeFromLeft (8);
    scSourceBox.setBounds (row.removeFromLeft (60));
}

juce::Slider& HardCapEditor::addSlider (juce::Slider::SliderStyle style,
                                        const juce::String& paramId,
                                        std::unique_ptr<SliderAttachment>& attachment)
{
    juce::Slider* target = nullptr;

    if (paramId == "pre")          target = &preSlider;
    else if (paramId == "output")  target = &outputSlider;
    else if (paramId == "ceiling") target = &ceilingKnob;
    else if (paramId == "filter")  target = &filterKnob;
    else if (paramId == "shape")   target = &shapeKnob;
    else                           target = &floorField;

    target->setSliderStyle (style);
    target->setTextBoxStyle (style == juce::Slider::LinearBar ? juce::Slider::NoTextBox
                                                             : juce::Slider::TextBoxBelow,
                             true, 92, 18);
    addAndMakeVisible (*target);
    attachment = std::make_unique<SliderAttachment> (proc.apvts, paramId, *target);

    return *target;
}

juce::ComboBox& HardCapEditor::addCombo (const juce::String& paramId,
                                         std::unique_ptr<ComboAttachment>& attachment)
{
    juce::ComboBox* target = nullptr;

    if (paramId == "slope")          target = &slopeBox;
    else if (paramId == "filterpos") target = &filterPosBox;
    else if (paramId == "sclink")    target = &scLinkBox;
    else                             target = &scSourceBox;

    addAndMakeVisible (*target);
    attachment = std::make_unique<ComboAttachment> (proc.apvts, paramId, *target);

    return *target;
}
