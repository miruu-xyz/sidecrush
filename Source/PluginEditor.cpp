#include "PluginEditor.h"

#include <BinaryData.h>

#include <limits>

//==============================================================================
juce::Font hcFont (float pointHeight)
{
    // ponytail: a function-local static Typeface, released during static
    // destruction. Move it into the LookAndFeel if the leak detector ever
    // starts complaining about the shutdown ordering.
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::ZalandoSansExpanded_ttf, BinaryData::ZalandoSansExpanded_ttfSize);

    return juce::Font (juce::FontOptions (typeface).withPointHeight (pointHeight));
}

void paintWell (juce::Graphics& g, juce::Rectangle<float> r, float corner,
                juce::Colour centre, juce::Colour edge)
{
    g.setGradientFill ({ centre, r.getCentreX(), r.getCentreY(),
                         edge, r.getX(), r.getY(), true });
    g.fillRoundedRectangle (r, corner);

    // The design's `inset 1px 1px 4px rgba(0,0,0,0.25)`. JUCE has no inner
    // shadow, so this is the same idea by hand: the outline redrawn a few times
    // just inside the shape, offset down and right, and clipped to it.
    juce::Path shape;
    shape.addRoundedRectangle (r, corner);

    g.saveState();
    g.reduceClipRegion (shape);
    g.setColour (juce::Colours::black.withAlpha (0.09f));

    for (int i = 0; i < 4; ++i)
        g.strokePath (shape, juce::PathStrokeType (1.0f + 2.0f * (float) i),
                      juce::AffineTransform::translation (1.0f, 1.0f));

    g.restoreState();
}

//==============================================================================
HardCapLookAndFeel::HardCapLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, hccolour::value);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, hccolour::accent.withAlpha (0.3f));
    setColour (juce::Slider::rotarySliderFillColourId, hccolour::accent);
    setColour (juce::CaretComponent::caretColourId, hccolour::accent);
    setColour (juce::PopupMenu::backgroundColourId, hccolour::wellCentre);
    setColour (juce::PopupMenu::textColourId, hccolour::value);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, hccolour::accent.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, hccolour::value);
}

// Every control's bounds are given in design coordinates, so the split between
// the dial and its readout is a fixed rule rather than something negotiated
// from the text box's requested size: a dial takes a square off the top, a
// fader takes everything but the bottom 32px, and whatever is left is the
// readout. That lands both readouts on the design's baseline.
juce::Slider::SliderLayout HardCapLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    const auto bounds = slider.getLocalBounds();
    juce::Slider::SliderLayout layout;

    if (slider.getTextBoxPosition() == juce::Slider::NoTextBox)
    {
        layout.sliderBounds = bounds;
        return layout;
    }

    const auto sliderHeight = slider.isRotary() ? bounds.getWidth()
                                                : bounds.getHeight() - 32;

    layout.sliderBounds = bounds.withHeight (sliderHeight);
    layout.textBoxBounds = bounds.withTop (sliderHeight);
    return layout;
}

void HardCapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                           float pos, float startAngle, float endAngle,
                                           juce::Slider& slider)
{
    const auto centre = juce::Rectangle<int> (x, y, w, h).getCentre().toFloat();
    const auto radius = (float) juce::jmin (w, h) * 0.5f - knobMargin;
    const auto angle = startAngle + pos * (endAngle - startAngle);

    juce::Path body;
    body.addEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

    // Figma: offset (3,4), 5.85 blur, black at 25%. Identical on every dial --
    // the small ones are not scaled-down copies of the big one.
    juce::DropShadow { juce::Colours::black.withAlpha (0.25f), 10, { 3, 4 } }.drawForPath (g, body);

    g.setGradientFill ({ hccolour::knobTop,    centre.x - radius * 0.16f, centre.y - radius,
                         hccolour::knobBottom, centre.x + radius * 0.16f, centre.y + radius, false });
    g.fillPath (body);

    // The pointer runs from 0.386R to the rim on all three dials, and is 3px
    // wide on the big one against 2px on the small ones -- which is the same
    // 3.75% of the radius, floored so it never thins out to nothing.
    const auto inner = radius * 0.386f;
    const auto thickness = juce::jmax (2.0f, radius * 0.0375f);

    juce::Path pointer;
    pointer.addLineSegment ({ centre.x + inner * std::sin (angle),
                              centre.y - inner * std::cos (angle),
                              centre.x + radius * std::sin (angle),
                              centre.y - radius * std::cos (angle) }, thickness);

    const auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);

    // A lit pointer bleeds cyan into the knob face; an idle one is flat. Figma
    // draws that as a heavily blurred ellipse lying along the pointer.
    if (colour == hccolour::accent)
        juce::DropShadow { colour.withAlpha (0.55f), 14, {} }.drawForPath (g, pointer);

    g.setColour (colour);
    g.fillPath (pointer);
}

void HardCapLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                           float pos, float, float,
                                           juce::Slider::SliderStyle, juce::Slider&)
{
    const auto area = juce::Rectangle<int> (x, y, w, h).toFloat();

    g.setColour (hccolour::track);
    g.fillRect (area.getCentreX() - 1.0f, area.getY(), 2.0f, area.getHeight());

    // The cap overhangs the ends of the track by half its height, exactly as it
    // does in the design at either extreme of travel.
    const juce::Rectangle<float> cap { area.getX(), pos - 5.5f, area.getWidth(), 11.0f };

    g.setGradientFill ({ hccolour::thumbTop,    cap.getTopLeft(),
                         hccolour::thumbBottom, cap.getBottomRight(), false });
    g.fillRoundedRectangle (cap, 3.0f);

    g.setColour (juce::Colours::black.withAlpha (0.2f));

    for (auto row : { 3.5f, 5.5f, 7.5f })
        g.fillRect (cap.getX() + 7.0f, cap.getY() + row - 0.5f, cap.getWidth() - 14.0f, 1.0f);
}

juce::Label* HardCapLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (hcFont (12.0f));
    label->setJustificationType (juce::Justification::centredBottom);
    label->setBorderSize ({ 0, 0, 3, 0 });
    return label;
}

void HardCapLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    const juce::Rectangle<float> r { 0.0f, 0.0f, (float) w, (float) h };
    paintWell (g, r, 4.0f, hccolour::wellCentre, hccolour::wellEdge);
    g.setColour (hccolour::hairline);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
}

//==============================================================================
Pill::Pill (HardCapProcessor& p, const char* paramId, Gesture g, juce::String dimPrefix)
    : param (*p.apvts.getParameter (paramId)),
      attachment (param, [this] (float) { repaint(); }),
      gesture (g), prefix (std::move (dimPrefix))
{
    setComponentID (paramId);
}

void Pill::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto engaged = onTint.isOpaque() && param.getValue() > 0.5f;

    if (engaged)
    {
        // Figma tints the well itself red rather than only the border, and hangs
        // a wide soft glow off it.
        juce::Path shape;
        shape.addRoundedRectangle (r, 4.0f);
        juce::DropShadow { onTint.withAlpha (0.30f), 24, {} }.drawForPath (g, shape);

        paintWell (g, r, 4.0f, juce::Colour { 0xff740000 }.withAlpha (0.55f),
                   hccolour::wellEdge);
    }
    else
    {
        paintWell (g, r, 4.0f, hccolour::wellCentre, hccolour::wellEdge);
    }

    if (hovered)
    {
        juce::Path shape;
        shape.addRoundedRectangle (r, 4.0f);
        juce::DropShadow { hccolour::accent.withAlpha (0.10f), 26, {} }.drawForPath (g, shape);
    }

    const auto border = engaged  ? hccolour::clipBorder
                      : hovered  ? hccolour::accent
                      : outlined ? hccolour::hairline
                                 : juce::Colours::transparentBlack;

    if (! border.isTransparent())
    {
        g.setColour (border);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    const auto text = overrideText ? overrideText() : param.getCurrentValueAsText();

    g.setFont (hcFont (12.0f));

    const auto dormant = outlined && ! engaged;

    if (prefix.isEmpty())
    {
        g.setColour (engaged ? hccolour::clipOn : dormant ? hccolour::label : hccolour::value);
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
        return;
    }

    // "FILTER PRE" and "SIGNAL EXT": the name stays dim, the value does not.
    const auto font = hcFont (12.0f);
    const auto gap = juce::GlyphArrangement::getStringWidth (font, " ");
    const auto prefixWidth = juce::GlyphArrangement::getStringWidth (font, prefix);
    const auto total = prefixWidth + gap + juce::GlyphArrangement::getStringWidth (font, text);
    const auto left = ((float) getWidth() - total) * 0.5f;

    g.setColour (hccolour::label);
    g.drawText (prefix, juce::Rectangle<float> { left, 0.0f, prefixWidth, (float) getHeight() },
                juce::Justification::centredLeft);

    g.setColour (hccolour::value);
    g.drawText (text, juce::Rectangle<float> { left + prefixWidth + gap, 0.0f,
                                               total - prefixWidth - gap, (float) getHeight() },
                juce::Justification::centredLeft);
}

void Pill::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void Pill::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void Pill::mouseDown (const juce::MouseEvent&)
{
    if (gesture != Gesture::drag)
        return;

    dragging = true;
    valueAtDragStart = param.getValue();
    attachment.beginGesture();
}

void Pill::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    // 150px of travel covers the whole range, which is enough resolution for
    // eight slope steps and fine enough for the floor's 0.1 dB interval.
    const auto moved = valueAtDragStart - (float) e.getDistanceFromDragStartY() / 150.0f;
    attachment.setValueAsPartOfGesture (param.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, moved)));
}

void Pill::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        dragging = false;
        attachment.endGesture();
        return;
    }

    if (gesture != Gesture::cycle || ! e.mouseWasClicked())
        return;

    const auto steps = juce::jmax (2, param.getNumSteps());
    const auto current = juce::roundToInt (param.getValue() * (float) (steps - 1));
    const auto next = (float) ((current + 1) % steps) / (float) (steps - 1);

    attachment.setValueAsCompleteGesture (param.convertFrom0to1 (next));
}

//==============================================================================
FilterLabel::FilterLabel (HardCapProcessor& p) : proc (p)
{
    setComponentID ("filterlabel");
}

void FilterLabel::paint (juce::Graphics& g)
{
    auto& param = *proc.apvts.getParameter (ids::filterPos);

    g.setFont (hcFont (12.0f));
    g.setColour (hovered ? hccolour::accent : hccolour::label);
    g.drawText (hovered ? param.getCurrentValueAsText() : "FILTER",
                getLocalBounds(), juce::Justification::centred);
}

void FilterLabel::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void FilterLabel::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void FilterLabel::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    auto& param = *proc.apvts.getParameter (ids::filterPos);

    param.beginChangeGesture();
    param.setValueNotifyingHost (param.getValue() > 0.5f ? 0.0f : 1.0f);
    param.endChangeGesture();
    repaint();
}

//==============================================================================
IconButton::IconButton (const void* svgData, int svgSize)
{
    // The exported glyphs carry the design's pre-blend #b1b1b1; inside the scope
    // that dodges to hccolour::hairline, and brightens under the pointer.
    const auto recolour = [svgData, svgSize] (juce::Colour to)
    {
        auto d = juce::Drawable::createFromImageData (svgData, (size_t) svgSize);

        if (d != nullptr)
            d->replaceColour (juce::Colour { 0xffb1b1b1 }, to);

        return d;
    };

    resting = recolour (hccolour::hairline.brighter (0.6f));
    lit = recolour (hccolour::value);
}

void IconButton::paint (juce::Graphics& g)
{
    if (auto* d = hovered ? lit.get() : resting.get())
        d->drawWithin (g, getLocalBounds().toFloat(),
                       juce::RectanglePlacement::centred
                           | juce::RectanglePlacement::doNotResize, 1.0f);
}

void IconButton::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void IconButton::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onClick != nullptr)
        onClick();
}

//==============================================================================
ActivityLed::ActivityLed (HardCapProcessor& p) : processor (p) {}

void ActivityLed::refresh()
{
    const auto target = processor.gainReduction.load (std::memory_order_relaxed);
    const auto decayed = juce::jmax (target, level * 0.72f); // fast attack, visible decay

    if (std::abs (decayed - level) > 0.005f)
    {
        level = decayed;
        repaint();
    }
}

void ActivityLed::paint (juce::Graphics& g)
{
    const auto centre = getLocalBounds().getCentre().toFloat();
    const auto lit = juce::jlimit (0.0f, 1.0f, level);

    juce::Path dot;
    dot.addEllipse (centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);

    // Two stacked glows in the design: a tight bright one and a wide faint one.
    juce::DropShadow { hccolour::accent.withAlpha (0.15f + 0.85f * lit), 12, {} }.drawForPath (g, dot);
    juce::DropShadow { hccolour::accent.withAlpha (0.10f + 0.60f * lit), 5, {} }.drawForPath (g, dot);

    g.setColour (hccolour::accent.withAlpha (0.35f + 0.65f * lit));
    g.fillPath (dot);
}

//==============================================================================
ScopeComponent::ScopeComponent (HardCapProcessor& p) : processor (p) {}

void ScopeComponent::refresh()
{
    snapshotHead = processor.scope.head();
    repaint();
}

void ScopeComponent::setShapePreview (bool shouldShow)
{
    if (shapePreview == shouldShow)
        return;

    shapePreview = shouldShow;
    repaint();
}

void ScopeComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    paintWell (g, bounds, 12.0f, hccolour::scopeCentre, hccolour::scopeEdge);

    juce::Path frame;
    frame.addRoundedRectangle (bounds, 12.0f);

    g.saveState();
    g.reduceClipRegion (frame);

    const auto mid = bounds.getCentreY();
    const auto amp = bounds.getHeight() * 0.5f - 10.0f;
    const auto toY = [&] (float v) { return mid - juce::jlimit (-1.2f, 1.2f, v) * amp; };

    // ---- ceiling and floor, as the bands from the "Threshold lines" variant --
    // Derived here rather than mirrored out of the engine: the engine only
    // refreshes its copy inside processBlock, so in a stopped host the bands
    // would sit at their defaults until playback started. This is the same
    // arithmetic pullParameters does.
    const auto floorDb = processor.apvts.getRawParameterValue (ids::floorDb)->load();
    const auto ceilingLin = juce::Decibels::decibelsToGain (
        processor.apvts.getRawParameterValue (ids::ceiling)->load());
    const auto floorLin = floorDb <= floorOffDb ? 0.0f
                                               : juce::Decibels::decibelsToGain (floorDb);

    // Figma's layer order matters here and is not the obvious one: the zero line
    // and the red floor band go *under* the traces, and the cyan lid bands go
    // over the top of them. Drawing the lid bands first instead loses the tint
    // where a trace crosses into the clamped region, which is the one place the
    // overlay is actually telling you something.

    // Snapped to a whole row: at 1x a half-pixel line is drawn as two rows at
    // half strength, which reads as a smudge rather than the design's hairline.
    g.setColour (hccolour::scopeLine);
    g.fillRect (bounds.getX(), std::floor (mid) - 1.0f, bounds.getWidth(), 1.0f);

    if (floorLin > 0.0f)
    {
        g.setColour (hccolour::clipOn.withAlpha (0.2f));
        g.fillRect (bounds.getX(), toY (floorLin), bounds.getWidth(), toY (-floorLin) - toY (floorLin));
    }

    // ---- the SHAPE dial's preview, drawn whether or not audio is running -----
    if (shapePreview)
    {
        const auto shape = processor.apvts.getRawParameterValue (ids::shape)->load();
        const auto exponent = std::pow (2.0f, -4.0f * shape);

        juce::Path up, down;

        for (int i = 0; i <= 96; ++i)
        {
            const auto t = (float) i / 96.0f;
            const auto lid = 1.0f - std::pow (t, exponent);
            const auto px = bounds.getX() + t * bounds.getWidth();

            i == 0 ? up.startNewSubPath (px, toY (lid)) : up.lineTo (px, toY (lid));
            i == 0 ? down.startNewSubPath (px, toY (-lid)) : down.lineTo (px, toY (-lid));
        }

        g.setColour (hccolour::accent.withAlpha (0.45f));
        g.strokePath (up, juce::PathStrokeType (1.2f));
        g.strokePath (down, juce::PathStrokeType (1.2f));
    }

    // ---- traces -------------------------------------------------------------
    const auto& fifo = processor.scope;
    const auto head = snapshotHead;

    if (head >= 512)
    {
        // Trigger on the most recent rising crossing of the trace's own mean.
        // In PRE the detector is bipolar and the mean is ~0, i.e. a zero
        // crossing. In POST it is a rectified envelope that never goes negative,
        // so a zero crossing could only fire where the clamp bottomed out and
        // the display would free-run instead of latching. The mean works for both.
        const auto maxScan = (int64_t) juce::jmin (ScopeFifo::capacity - 8, 12000);
        const auto scanFrom = juce::jmax<int64_t> (2, head - maxScan);

        auto sum = 0.0;

        for (int64_t i = scanFrom; i < head; ++i)
            sum += (double) fifo.at (i).sc;

        const auto level = (float) (sum / (double) juce::jmax<int64_t> (1, head - scanFrom));

        int64_t trigger = head - 1;
        int64_t previous = -1;
        int found = 0;

        for (int64_t i = head - 2; i > scanFrom; --i)
        {
            if (fifo.at (i - 1).sc <= level && fifo.at (i).sc > level)
            {
                if (found == 0) trigger = i;
                else { previous = i; break; }

                ++found;
            }
        }

        // Auto timebase: two cycles of whatever the sidechain is doing, so a
        // 40 Hz sub and a 100 Hz sub fill the window the same way.
        int64_t window = 1024;

        if (previous > 0 && trigger > previous)
            window = juce::jlimit<int64_t> (128, 8192, (trigger - previous) * 2);

        const auto startIndex = juce::jmax<int64_t> (1, trigger - window);
        const auto count = trigger - startIndex;

        if (count >= 8)
        {
            // Two cycles of a 40 Hz sub is ~2400 samples across 380 pixels, so a
            // polyline through every sixth sample is what actually gets drawn --
            // it misses the peaks, and which samples it lands on shifts frame to
            // frame, so the carrier crawls. Each column is drawn as the range of
            // the samples inside it instead, joined to its neighbour so the
            // result reads as one continuous trace.
            const auto columns = juce::jmax (1, (int) bounds.getWidth());

            const auto drawTrace = [&] (auto value, juce::Colour colour, float thickness)
            {
                juce::Path path;
                auto previousLow = 0.0f, previousHigh = 0.0f;
                auto started = false;

                for (int column = 0; column < columns; ++column)
                {
                    const auto from = startIndex + count * column / columns;
                    const auto to = juce::jmax (from + 1, startIndex + count * (column + 1) / columns);

                    auto low = std::numeric_limits<float>::max();
                    auto high = std::numeric_limits<float>::lowest();

                    for (auto i = from; i < to; ++i)
                    {
                        const auto v = value (fifo.at (i));
                        low = juce::jmin (low, v);
                        high = juce::jmax (high, v);
                    }

                    if (started)
                    {
                        low = juce::jmin (low, previousHigh);
                        high = juce::jmax (high, previousLow);
                    }

                    const auto px = bounds.getX() + (float) column + 0.5f;
                    path.startNewSubPath (px, toY (high));
                    path.lineTo (px, toY (low));

                    previousLow = low;
                    previousHigh = high;
                    started = true;
                }

                g.setColour (colour);
                g.strokePath (path, juce::PathStrokeType (thickness));
            };

            // The lid is what the carrier is being squashed against, so it is
            // drawn as the aperture it actually is -- mirrored either side of
            // zero, with the output visibly slamming into it.
            const auto lidColour = hccolour::hairline.brighter (0.7f);
            drawTrace ([] (const ScopeFrame& f) { return  f.lid; }, lidColour, 1.2f);
            drawTrace ([] (const ScopeFrame& f) { return -f.lid; }, lidColour, 1.2f);

            drawTrace ([] (const ScopeFrame& f) { return f.sc; },  hccolour::accent, 1.6f);
            drawTrace ([] (const ScopeFrame& f) { return f.out; }, hccolour::output, 1.4f);
        }
    }

    for (auto sign : { 1.0f, -1.0f })
    {
        const auto edge = sign > 0.0f ? bounds.getY() : bounds.getBottom();
        const auto threshold = toY (sign * ceilingLin);
        const juce::Rectangle<float> band { bounds.getX(), juce::jmin (edge, threshold),
                                            bounds.getWidth(), std::abs (threshold - edge) };

        g.setGradientFill ({ hccolour::accent.withAlpha (0.07f), bounds.getCentreX(), edge,
                             hccolour::accent.withAlpha (0.14f), bounds.getCentreX(), threshold, false });
        g.fillRect (band);
    }

    g.restoreState();

    g.setFont (hcFont (12.0f));
    g.setColour (hccolour::brand);
    g.drawText ("HARDCAP", bounds.reduced (10.0f).translated (0.0f, 3.0f),
                juce::Justification::bottomLeft);

    g.setColour (hccolour::brandDim);
    g.drawText ("by miruu", bounds.reduced (10.0f).translated (0.0f, 3.0f)
                                  .withTrimmedLeft (juce::GlyphArrangement::getStringWidth (hcFont (12.0f), "HARDCAP ")),
                juce::Justification::bottomLeft);
}

//==============================================================================
SettingsPanel::SettingsPanel (HardCapProcessor& p)
    : link     (p, ids::scLink,    Pill::Gesture::cycle),
      hq       (p, ids::hq,        Pill::Gesture::cycle),
      filterPos (p, ids::filterPos, Pill::Gesture::cycle, "FILTER"),
      source   (p, ids::scSource,  Pill::Gesture::cycle, "SIGNAL")
{
    // AudioParameterBool reads out as "On"/"Off"; the design names the modes.
    hq.overrideText = [&p] { return p.apvts.getRawParameterValue (ids::hq)->load() > 0.5f
                                        ? juce::String ("HQ") : juce::String ("LQ"); };

    for (auto* pill : { &link, &hq, &filterPos, &source })
        addAndMakeVisible (pill);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Figma lifts the whole panel to a lighter slate while it is open.
    paintWell (g, bounds, 12.0f, juce::Colour { 0xff151c22 }, juce::Colour { 0xff2a3844 });

    g.setFont (hcFont (12.0f));
    g.setColour (hccolour::brand);
    g.drawText ("HARDCAP", bounds.reduced (10.0f).translated (0.0f, 3.0f),
                juce::Justification::bottomLeft);

    g.setColour (hccolour::brandDim.brighter (0.4f));
    g.drawText ("by miruu", bounds.reduced (10.0f).translated (0.0f, 3.0f)
                                  .withTrimmedLeft (juce::GlyphArrangement::getStringWidth (hcFont (12.0f), "HARDCAP ")),
                juce::Justification::bottomLeft);
}

void SettingsPanel::resized()
{
    // Two centred rows, 8px apart, sized to their own text as in the design.
    const auto centreX = getWidth() / 2;
    const auto centreY = getHeight() / 2;

    const auto place = [] (Pill& a, int aWidth, Pill& b, int bWidth, int cx, int y)
    {
        const auto total = aWidth + 8 + bWidth;
        a.setBounds (cx - total / 2, y, aWidth, 21);
        b.setBounds (cx - total / 2 + aWidth + 8, y, bWidth, 21);
    };

    place (link, 80, hq, 43, centreX, centreY - 25);
    place (filterPos, 101, source, 107, centreX, centreY + 4);
}

//==============================================================================
HardCapEditor::HardCapEditor (HardCapProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      slopePill (p, ids::slope,   Pill::Gesture::drag),
      floorPill (p, ids::floorDb, Pill::Gesture::drag),
      clipPill  (p, ids::clip,    Pill::Gesture::cycle),
      filterLabel (p),
      gear  (BinaryData::settings_svg, BinaryData::settings_svgSize),
      close (BinaryData::close_svg,    BinaryData::close_svgSize),
      led (p), scope (p), settings (p)
{
    setLookAndFeel (&lookAndFeel);

    addSlider (preSlider,    juce::Slider::LinearVertical,     ids::pre,      {}, true, preAtt);
    addSlider (outputSlider, juce::Slider::LinearVertical,     ids::output,   {}, true, outputAtt);
    addSlider (ceilingKnob,  juce::Slider::RotaryVerticalDrag, ids::ceiling,  hccolour::accent, true, ceilingAtt);
    addSlider (filterKnob,   juce::Slider::RotaryVerticalDrag, ids::filterHz, hccolour::accent, false, filterAtt);
    addSlider (shapeKnob,    juce::Slider::RotaryVerticalDrag, ids::shape,    hccolour::accent, false, shapeAtt);

    // The design's dials sweep 270 degrees: 7:30 round to 4:30. JUCE measures
    // clockwise from 12 o'clock and requires both angles to be positive, so the
    // end angle is expressed past a full turn rather than as a negative start.
    for (auto* knob : { &ceilingKnob, &filterKnob, &shapeKnob })
        knob->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);

    // "This shows OFF if the Filter is off."
    slopePill.overrideText = [this]
    {
        const auto hz = proc.apvts.getRawParameterValue (ids::filterHz)->load();
        return hz >= filterOffHz - 1.0f ? juce::String ("OFF")
                                        : proc.apvts.getParameter (ids::slope)->getCurrentValueAsText();
    };

    clipPill.setOutlined (true);
    clipPill.onTint = hccolour::clipOn;
    clipPill.overrideText = [] { return juce::String ("CLIP"); };

    // The scope is only ever one of the two, at the same bounds, so the panel is
    // a swap rather than an overlay -- that is how the Figma variant reads.
    addAndMakeVisible (scope);
    addChildComponent (settings);

    addAndMakeVisible (led);
    addAndMakeVisible (slopePill);
    addAndMakeVisible (floorPill);
    addAndMakeVisible (clipPill);
    addAndMakeVisible (gear);
    addChildComponent (close);

    // After the dial, so it wins the clicks inside the dial's padded bounds.
    addAndMakeVisible (filterLabel);

    gear.onClick = [this] { showSettings (true); };
    close.onClick = [this] { showSettings (false); };

    // "While hovering and dragging the Display screen should show ramp as it
    // would be applied."
    shapeKnob.onDragStart = [this] { scope.setShapePreview (true); };
    shapeKnob.onDragEnd = [this] { scope.setShapePreview (false); };

    lastFilterPost = proc.filterIsPost.load (std::memory_order_relaxed);

    const auto stored = (float) proc.apvts.state.getProperty ("uiScale", 1.0);
    setScale (juce::jlimit (0.75f, 1.5f, stored));

    // One timer for both animated children. 30 is plenty for a scope and costs
    // half of 60; it runs for as long as the editor is open, whether or not the
    // host is playing anything.
    startTimerHz (30);
}

HardCapEditor::~HardCapEditor()
{
    setLookAndFeel (nullptr);
}

void HardCapEditor::addSlider (juce::Slider& slider, juce::Slider::SliderStyle style,
                               const char* paramId, juce::Colour pointer, bool withReadout,
                               std::unique_ptr<SliderAttachment>& attachment)
{
    slider.setSliderStyle (style);
    slider.setComponentID (paramId);
    addAndMakeVisible (slider);

    // Must run after addAndMakeVisible: the text box's Label is created here,
    // and without a parent it silently picks up JUCE's default LookAndFeel
    // rather than this one.
    slider.setTextBoxStyle (withReadout ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                            false, 60, 18);

    if (! pointer.isTransparent())
        slider.setColour (juce::Slider::rotarySliderFillColourId, pointer);

    attachment = std::make_unique<SliderAttachment> (proc.apvts, paramId, slider);
}

void HardCapEditor::setScale (float scale)
{
    scaleFactor = scale;
    proc.apvts.state.setProperty ("uiScale", scale, nullptr);

    setTransform (juce::AffineTransform::scale (scale));
    setSize (designWidth, designHeight);
}

void HardCapEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel);

    for (auto percent : { 75, 100, 125, 150 })
        menu.addItem (percent, juce::String (percent) + "%", true,
                      juce::approximatelyEqual (scaleFactor, (float) percent / 100.0f));

    menu.showMenuAsync (juce::PopupMenu::Options {}.withTargetComponent (this),
                        [safe = juce::Component::SafePointer<HardCapEditor> (this)] (int choice)
                        {
                            if (choice > 0 && safe != nullptr)
                                safe->setScale ((float) choice / 100.0f);
                        });
}

void HardCapEditor::showSettings (bool shouldShow)
{
    scope.setVisible (! shouldShow);
    settings.setVisible (shouldShow);
    gear.setVisible (! shouldShow);
    close.setVisible (shouldShow);
    clipPill.setVisible (! shouldShow);
}

void HardCapEditor::refreshFromParameters()
{
    for (auto* slider : { &preSlider, &outputSlider, &ceilingKnob, &filterKnob, &shapeKnob })
        slider->updateText();

    timerCallback();
    repaint();
}

void HardCapEditor::timerCallback()
{
    if (scope.isVisible())
        scope.refresh();

    led.refresh();

    // The FILTER text function reads this flag, so the readout only changes when
    // something asks the dial to rebuild its text. Written from here as well as
    // from the audio thread: same value either way, and this path still works in
    // a host that is not currently processing.
    if (const auto post = proc.apvts.getRawParameterValue (ids::filterPos)->load() > 0.5f;
        post != lastFilterPost)
    {
        lastFilterPost = post;
        proc.filterIsPost.store (post, std::memory_order_relaxed);
        filterKnob.updateText();
    }

    // The dial's pointer goes flat when its filter is switched off -- the design
    // calls it out as the only dial that does this.
    const auto filterOff = proc.apvts.getRawParameterValue (ids::filterHz)->load() >= filterOffHz - 1.0f;
    const auto wanted = filterOff ? hccolour::idle : hccolour::accent;

    if (filterKnob.findColour (juce::Slider::rotarySliderFillColourId) != wanted)
    {
        filterKnob.setColour (juce::Slider::rotarySliderFillColourId, wanted);
        slopePill.repaint();
    }
}

void HardCapEditor::paint (juce::Graphics& g)
{
    g.fillAll (hccolour::background);

    // `inset 8px 10px 22.2px rgba(255,255,255,0.02)` -- a barely-there lift in
    // the top-left corner, but it is measurably there in the reference render.
    g.setGradientFill ({ juce::Colours::white.withAlpha (0.021f), 0.0f, 0.0f,
                         juce::Colours::transparentWhite, 260.0f, 200.0f, true });
    g.fillRect (getLocalBounds());

    // The scope's `0 0 180px rgba(78,228,251,0.05)`. A real 180px blur here
    // would cost far more than the whole rest of this paint, and the result is
    // a soft round falloff either way.
    const juce::Rectangle<float> scopeArea { 451.0f, 48.0f, 380.0f, 230.0f };
    g.setGradientFill ({ hccolour::accent.withAlpha (0.055f), scopeArea.getCentre(),
                         juce::Colours::transparentBlack,
                         scopeArea.getCentre().translated (0.0f, scopeArea.getHeight() * 1.6f), true });
    g.fillRect (scopeArea.expanded (110.0f));

    g.setFont (hcFont (16.0f));
    g.setColour (hccolour::label);
    g.drawText ("PRE",     juce::Rectangle<int> {  48, 48,  42, 19 }, juce::Justification::centred);
    g.drawText ("CEILING", juce::Rectangle<int> { 164, 48,  77, 19 }, juce::Justification::centred);
    g.drawText ("OUT",     juce::Rectangle<int> { 878, 48,  42, 19 }, juce::Justification::centred);

    g.setFont (hcFont (12.0f));
    g.drawText ("SHAPE", juce::Rectangle<int> { 334, 176, 50, 13 }, juce::Justification::centred);
}

void HardCapEditor::resized()
{
    // Every number here is read straight off Figma node 1:11 at 1:1. Dials are
    // given HardCapLookAndFeel::knobMargin of padding on every side so their
    // drop shadow and the pointer's glow are not clipped by their own bounds.
    preSlider.setBounds    (  48,  83,  42, 192); // track 83..243, readout to 275
    outputSlider.setBounds ( 878,  83,  42, 192);

    ceilingKnob.setBounds  ( 113,  69, 188, 206); // r 80 at (207,163)
    filterKnob.setBounds   ( 320,  66,  78,  78); // r 25 at (359,105)
    shapeKnob.setBounds    ( 320, 181,  78,  78); // r 25 at (359,220)

    led.setBounds          ( 224,  33,  48,  48); // 4px dot at (248,57), rest is glow
    filterLabel.setBounds  ( 335, 137,  48,  13);

    slopePill.setBounds    ( 313,  48,  92,  21);
    floorPill.setBounds    ( 313, 257,  92,  21);

    scope.setBounds        ( 451,  48, 380, 230);
    settings.setBounds     ( 451,  48, 380, 230);

    gear.setBounds         ( 805,  58,  16,  16);
    close.setBounds        ( 805,  58,  16,  16);
    clipPill.setBounds     ( 768, 247,  53,  21);
}
