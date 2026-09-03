#include "PluginEditor.h"

#include <BinaryData.h>

#include <limits>
#include <utility>

namespace
{
    // The UI scales the editor offers, in the order the switch cycles them.
    constexpr float scaleSteps[] { 0.75f, 1.0f, 1.25f, 1.5f };

    // Every menu here is the same shape: a list, a tick on the current entry,
    // and an index handed back. `target` is what the menu hangs off and what
    // keeps it honest -- the callback is dropped if that component has gone,
    // which is also what makes it safe for the callback to touch the editor the
    // pill belongs to.
    void showPillMenu (juce::Component& target, const juce::StringArray& items, int current,
                       std::function<void (int)> chosen)
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&target.getLookAndFeel());

        for (int i = 0; i < items.size(); ++i)
            menu.addItem (i + 1, items[i], true, i == current);

        menu.showMenuAsync (juce::PopupMenu::Options {}.withTargetComponent (&target),
                            [safe = juce::Component::SafePointer<juce::Component> (&target),
                             pick = std::move (chosen)] (int choice)
                            {
                                if (choice > 0 && safe != nullptr)
                                    pick (choice - 1);
                            });
    }

    // RCTF's switch, shared by the settings pill and the MIX caption -- one
    // switch in two places, and it moves two parameters either way.
    //
    // MIX's midpoint is RCTF's "fully processed" the way the top of the travel
    // is the symmetric mode's, so turning it on puts the fader where this mode's
    // own full effect is rather than leaving it pointing at a blend that has
    // just changed meaning underneath it. Turning it off puts back whatever the
    // fader was doing before, which is kept in the plug-in's state tree: not a
    // parameter, so it stays out of the host's automation list, but saved with
    // the session, so reloading does not forget.
    void toggleRecti (SideCrushProcessor& p)
    {
        static constexpr auto stash = "mixBeforeRecti";

        auto& flag = *p.apvts.getParameter (ids::recti);
        auto& mix = *p.apvts.getParameter (ids::mix);

        const auto turningOn = flag.getValue() <= 0.5f;
        const auto current = mix.convertFrom0to1 (mix.getValue());

        if (turningOn)
            p.apvts.state.setProperty (stash, current, nullptr);

        const auto wanted = turningOn ? 50.0f
                                      : (float) p.apvts.state.getProperty (stash, 100.0);

        flag.beginChangeGesture();
        flag.setValueNotifyingHost (turningOn ? 1.0f : 0.0f);
        flag.endChangeGesture();

        mix.beginChangeGesture();
        mix.setValueNotifyingHost (mix.convertTo0to1 (wanted));
        mix.endChangeGesture();
    }
}

//==============================================================================
ScalePreference::ScalePreference()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "SideCrush";
    options.filenameSuffix      = "settings";
    options.folderName          = "miruu";
    options.osxLibrarySubFolder = "Application Support";

    file = std::make_unique<juce::PropertiesFile> (options);
}

float ScalePreference::get() const
{
    // Clamped to the offered steps: the file is user-editable, and a scale of 40
    // would leave the editor unreadable with no way back to the switch.
    return juce::jlimit (scaleSteps[0], scaleSteps[std::size (scaleSteps) - 1],
                         (float) file->getDoubleValue ("uiScale", 1.0));
}

void ScalePreference::set (float scale)
{
    // setValue is a no-op when the value has not changed, so this is safe to
    // call on every setScale, including the one that applies the stored value.
    file->setValue ("uiScale", (double) scale);
    file->saveIfNeeded();
}

//==============================================================================
juce::Font uiFont (float pointHeight)
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

void paintWordmark (juce::Graphics& g, juce::Rectangle<float> panel, juce::Colour dim)
{
    const auto font = uiFont (12.0f);
    // Figma puts the wordmark's baseline 10px above the panel's bottom edge.
    const auto area = panel.reduced (10.0f).translated (0.0f, 3.0f);

    g.setFont (font);
    g.setColour (uicolour::brand);
    g.drawText ("SIDECRUSH", area, juce::Justification::bottomLeft);

    g.setColour (dim);
    g.drawText ("by miruu",
                area.withTrimmedLeft (juce::GlyphArrangement::getStringWidth (font, "SIDECRUSH ")),
                juce::Justification::bottomLeft);
}

//==============================================================================
SideCrushLookAndFeel::SideCrushLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, uicolour::value);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, uicolour::accent.withAlpha (0.3f));
    setColour (juce::Slider::rotarySliderFillColourId, uicolour::accent);
    setColour (juce::CaretComponent::caretColourId, uicolour::accent);
    setColour (juce::PopupMenu::backgroundColourId, uicolour::wellCentre);
    setColour (juce::PopupMenu::textColourId, uicolour::value);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, uicolour::accent.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, uicolour::value);
}

// Every control's bounds are given in design coordinates, so the split between
// the dial and its readout is a fixed rule rather than something negotiated
// from the text box's requested size: a dial takes a square off the top, a
// fader takes everything but the bottom 32px, and whatever is left is the
// readout. That lands both readouts on the design's baseline.
juce::Slider::SliderLayout SideCrushLookAndFeel::getSliderLayout (juce::Slider& slider)
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

    // A fader's cap overhangs the ends of its track by half its height, so the
    // travel has to stop 6px short of the component or the cap clips at the top
    // of the stroke at maximum (and at the bottom at minimum).
    layout.sliderBounds = bounds.withHeight (sliderHeight)
                                .reduced (0, slider.isRotary() ? 0 : 6);
    layout.textBoxBounds = bounds.withTop (sliderHeight);
    return layout;
}

void SideCrushLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
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

    g.setGradientFill ({ uicolour::knobTop,    centre.x - radius * 0.16f, centre.y - radius,
                         uicolour::knobBottom, centre.x + radius * 0.16f, centre.y + radius, false });
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
    if (colour == uicolour::accent)
        juce::DropShadow { colour.withAlpha (0.55f), 14, {} }.drawForPath (g, pointer);

    g.setColour (colour);
    g.fillPath (pointer);
}

void SideCrushLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                           float pos, float, float,
                                           juce::Slider::SliderStyle, juce::Slider&)
{
    const auto area = juce::Rectangle<int> (x, y, w, h).toFloat();

    g.setColour (uicolour::track);
    g.fillRect (area.getCentreX() - 1.0f, area.getY(), 2.0f, area.getHeight());

    // The cap overhangs the ends of the track by half its height, exactly as it
    // does in the design at either extreme of travel.
    const juce::Rectangle<float> cap { area.getX(), pos - 5.5f, area.getWidth(), 11.0f };

    g.setGradientFill ({ uicolour::thumbTop,    cap.getTopLeft(),
                         uicolour::thumbBottom, cap.getBottomRight(), false });
    g.fillRoundedRectangle (cap, 3.0f);

    g.setColour (juce::Colours::black.withAlpha (0.2f));

    for (auto row : { 3.5f, 5.5f, 7.5f })
        g.fillRect (cap.getX() + 7.0f, cap.getY() + row - 0.5f, cap.getWidth() - 14.0f, 1.0f);
}

juce::Label* SideCrushLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (uiFont (12.0f));
    label->setJustificationType (juce::Justification::centredBottom);
    label->setBorderSize ({ 0, 0, 3, 0 });
    return label;
}

void SideCrushLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    const juce::Rectangle<float> r { 0.0f, 0.0f, (float) w, (float) h };
    paintWell (g, r, 4.0f, uicolour::wellCentre, uicolour::background);
    g.setColour (uicolour::hairline);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
}

//==============================================================================
Pill::Pill (SideCrushProcessor& p, const char* paramId, Gesture g, juce::String dimPrefix)
    : param (p.apvts.getParameter (paramId)), gesture (g), prefix (std::move (dimPrefix))
{
    setComponentID (paramId);
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float) { repaint(); });
}

Pill::Pill (juce::String dimPrefix)
    : gesture (Gesture::cycle), prefix (std::move (dimPrefix))
{
}

void Pill::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = param != nullptr && param->getValue() > 0.5f;
    const auto engaged = on && onTint.isOpaque();

    if (engaged)
    {
        // Figma tints the well itself red rather than only the border, and hangs
        // a wide soft glow off it.
        juce::Path shape;
        shape.addRoundedRectangle (r, 4.0f);
        juce::DropShadow { onTint.withAlpha (0.30f), 24, {} }.drawForPath (g, shape);

        paintWell (g, r, 4.0f, juce::Colour { 0xff740000 }.withAlpha (0.55f), uicolour::background);
    }
    else
    {
        paintWell (g, r, 4.0f, uicolour::wellCentre, uicolour::background);
    }

    if (hovered)
    {
        juce::Path shape;
        shape.addRoundedRectangle (r, 4.0f);
        juce::DropShadow { uicolour::accent.withAlpha (0.10f), 26, {} }.drawForPath (g, shape);
    }

    const auto border = engaged  ? uicolour::clipBorder
                      : hovered  ? uicolour::accent
                      : outlined ? uicolour::hairline
                                 : juce::Colours::transparentBlack;

    if (! border.isTransparent())
    {
        g.setColour (border);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    drawLabel (g, overrideText != nullptr ? overrideText()
                                          : param != nullptr ? param->getCurrentValueAsText()
                                                             : juce::String(),
               textColour != nullptr ? textColour() : uicolour::value);
}

void Pill::drawLabel (juce::Graphics& g, juce::String text, juce::Colour bright)
{
    const auto font = uiFont (12.0f);
    g.setFont (font);

    if (prefix.isEmpty())
    {
        g.setColour (bright);
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
        return;
    }

    // "FILTER PRE", "SIGNAL EXT", "SCALE 100%": the name stays dim, the value
    // does not.
    const auto gap = juce::GlyphArrangement::getStringWidth (font, " ");
    const auto prefixWidth = juce::GlyphArrangement::getStringWidth (font, prefix);
    const auto total = prefixWidth + gap + juce::GlyphArrangement::getStringWidth (font, text);
    const auto left = ((float) getWidth() - total) * 0.5f;

    g.setColour (uicolour::label);
    g.drawText (prefix, juce::Rectangle<float> { left, 0.0f, prefixWidth, (float) getHeight() },
                juce::Justification::centredLeft);

    g.setColour (bright);
    g.drawText (text, juce::Rectangle<float> { left + prefixWidth + gap, 0.0f,
                                               total - prefixWidth - gap, (float) getHeight() },
                juce::Justification::centredLeft);
}

void Pill::showChoiceMenu()
{
    // Empty unless the parameter really is a list: JUCE only builds the value
    // strings for a discrete one, so a continuous parameter is never asked to
    // name its hundreds of steps.
    const auto choices = param != nullptr ? param->getAllValueStrings() : juce::StringArray {};

    if (choices.size() < 2)
        return;

    const auto last = choices.size() - 1;

    showPillMenu (*this, choices, juce::roundToInt (param->getValue() * (float) last),
                  [this, last] (int index)
                  {
                      attachment->setValueAsCompleteGesture (
                          param->convertFrom0to1 ((float) index / (float) last));
                  });
}

void Pill::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void Pill::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void Pill::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        onRightClick != nullptr ? onRightClick() : showChoiceMenu();
        return;
    }

    if (gesture != Gesture::drag)
        return;

    dragTarget = chooseDragTarget != nullptr ? chooseDragTarget() : param;

    if (dragTarget == nullptr)
        return;

    // Raw gesture calls rather than the attachment: the attachment is bound to
    // the parameter this pill *displays*, and the one it drags is chosen here.
    // This is the message thread, so there is nothing to marshal.
    valueAtDragStart = dragTarget->getValue();
    dragTarget->beginChangeGesture();

    if (onDragActive != nullptr)
        onDragActive (true);
}

void Pill::mouseDrag (const juce::MouseEvent& e)
{
    if (dragTarget == nullptr)
        return;

    // 150px of travel covers the whole range, which is enough resolution for
    // eight slope steps and fine enough for the floor's 0.1 dB interval.
    const auto moved = valueAtDragStart - (float) e.getDistanceFromDragStartY() / 150.0f;
    dragTarget->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, moved));
    repaint();
}

void Pill::mouseUp (const juce::MouseEvent& e)
{
    if (dragTarget != nullptr)
    {
        dragTarget->endChangeGesture();
        dragTarget = nullptr;

        if (onDragActive != nullptr)
            onDragActive (false);
    }

    // A click is a press and release that moved nothing, so it can still mean
    // something on a pill that also drags -- that is how the slope selector both
    // sweeps the filter and opens its menu.
    //
    // Right-clicks are not clicks in that sense: mouseDown already gave one to
    // the menu and returned, but the release still arrives here, and acting on
    // it too would open the list and step past the value in one gesture. The
    // modifiers on a mouse-up are the ones from before the button came up, so
    // the right button is still set here.
    if (! e.mouseWasClicked() || e.mods.isPopupMenu())
        return;

    if (onClick != nullptr)
    {
        onClick();
        return;
    }

    if (gesture != Gesture::cycle || param == nullptr)
        return;

    const auto steps = juce::jmax (2, param->getNumSteps());
    const auto current = juce::roundToInt (param->getValue() * (float) (steps - 1));
    const auto next = (float) ((current + 1) % steps) / (float) (steps - 1);

    attachment->setValueAsCompleteGesture (param->convertFrom0to1 (next));
}

//==============================================================================
DialCaption::DialCaption (juce::String captionText, float fontHeight)
    : caption (std::move (captionText)), fontSize (fontHeight)
{
    setComponentID (caption.toLowerCase());
}

void DialCaption::paint (juce::Graphics& g)
{
    g.setFont (uiFont (fontSize));

    // Dragging wins over hovering: if the dial is moving, its number is the only
    // thing worth saying.
    if (valueText.isNotEmpty())
    {
        g.setColour (uicolour::value);
        g.drawText (valueText, getLocalBounds(), juce::Justification::centred);
        return;
    }

    // A click suppresses the reveal for as long as it asked for, so what is on
    // screen straight afterwards is the state the click produced.
    const auto showHover = hovered && hoverText != nullptr && ! isTimerRunning();
    const auto active = activeText != nullptr ? activeText() : juce::String();

    const auto text = showHover ? hoverText() : active.isNotEmpty() ? active : caption;

    g.setColour (textColour != nullptr ? textColour()
                                       : showHover ? uicolour::accent : uicolour::label);
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

void DialCaption::setValueText (juce::String text)
{
    if (valueText == text)
        return;

    valueText = std::move (text);
    repaint();
}

void DialCaption::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void DialCaption::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void DialCaption::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onClick != nullptr)
    {
        onClick();

        if (clickHoldMs > 0)
            startTimer (clickHoldMs);

        repaint();
    }
}

void DialCaption::timerCallback()
{
    stopTimer();
    repaint();
}

//==============================================================================
IconButton::IconButton (const void* svgData, int svgSize)
{
    // The exported glyphs carry the design's pre-blend #b1b1b1; inside the scope
    // that dodges to uicolour::hairline, and brightens under the pointer.
    const auto recolour = [svgData, svgSize] (juce::Colour to)
    {
        auto d = juce::Drawable::createFromImageData (svgData, (size_t) svgSize);

        if (d != nullptr)
            d->replaceColour (juce::Colour { 0xffb1b1b1 }, to);

        return d;
    };

    resting = recolour (uicolour::hairline.brighter (0.6f));
    lit = recolour (uicolour::value);
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
ActivityLed::ActivityLed (SideCrushProcessor& p) : processor (p) {}

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
    juce::DropShadow { uicolour::accent.withAlpha (0.15f + 0.85f * lit), 12, {} }.drawForPath (g, dot);
    juce::DropShadow { uicolour::accent.withAlpha (0.10f + 0.60f * lit), 5, {} }.drawForPath (g, dot);

    g.setColour (uicolour::accent.withAlpha (0.35f + 0.65f * lit));
    g.fillPath (dot);
}

//==============================================================================
ScopeComponent::ScopeComponent (SideCrushProcessor& p) : processor (p)
{
    setComponentID ("scope");
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

void ScopeComponent::refresh()
{
    snapshotHead = processor.scope.head();
    repaint();
}

void ScopeComponent::setOverlay (Overlay wanted)
{
    if (overlay == wanted)
        return;

    overlay = wanted;
    repaint();
}

void ScopeComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    auto& ceiling = *processor.apvts.getParameter (ids::ceiling);
    ceilingAtDragStart = ceiling.getValue();
    ceiling.beginChangeGesture();
    dragging = true;

    if (onDragActive != nullptr)
        onDragActive (true);
}

void ScopeComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    // CEILING is tapered to amplitude and this axis *is* amplitude, so a pixel
    // of drag is a pixel of threshold: whatever the distance between the band's
    // edge and where you want it, that is how far you drag. No sensitivity
    // constant to pick, because the display already fixes the scale.
    const auto amp = (float) getHeight() * 0.5f - 10.0f;
    const auto moved = ceilingAtDragStart - (float) e.getDistanceFromDragStartY() / amp;

    processor.apvts.getParameter (ids::ceiling)
             ->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, moved));
}

void ScopeComponent::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    processor.apvts.getParameter (ids::ceiling)->endChangeGesture();

    if (onDragActive != nullptr)
        onDragActive (false);
}

void ScopeComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    paintWell (g, bounds, 12.0f, uicolour::scopeCentre, uicolour::scopeEdge);

    juce::Path frame;
    frame.addRoundedRectangle (bounds, 12.0f);

    g.saveState();
    g.reduceClipRegion (frame);

    const auto mid = bounds.getCentreY();
    const auto amp = bounds.getHeight() * 0.5f - 10.0f;
    const auto toY = [&] (float v) { return mid - juce::jlimit (-1.2f, 1.2f, v) * amp; };

    if (overlay == Overlay::shape)
    {
        // SHAPE is a transfer curve, not a waveform. x is how far the detector
        // has crossed the FLOOR..CEILING window; y is how far the lid has closed
        // by then. Nothing here is bipolar, so the centre line stays out of it.
        //
        // Plotted on a square, centred, so that SHAPE 0 is a true 45 degrees and
        // the two halves of the control read as the reflections they are: the
        // exponent is 2^(-4*shape), so +s and -s give t^p against t^(1/p), which
        // are mirror images about that diagonal. On a 380x230 frame they would
        // not be.
        shapeCurve.setShape (processor.apvts.getRawParameterValue (ids::shape)->load());

        const auto side = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 80.0f;
        const auto plot = juce::Rectangle<float> { side, side }.withCentre (bounds.getCentre());

        // Deliberately a guide rather than a plot. The engine's curve is
        // 1 - t^p, which has a single knee; what is drawn is that half-curve
        // together with its own 180-degree rotation about the centre of the
        // square, so it has two. That is the shape the control *sounds* like --
        // a break that arrives late and eases out, or one that slams in and
        // holds -- and at the extremes it squares off into a step, which is
        // exactly what those settings do to the audio.
        //
        // The doubling is exact at the joins and cannot change the ends: both
        // halves meet at (0.5, 0.5), the curve still leaves (0,0) and arrives at
        // (1,1), and at SHAPE 0 the exponent is 1 and both halves collapse back
        // onto the same straight diagonal. So the reading "left of centre is
        // gentler, right of centre is harder" stays honest even though the
        // curvature is not the transfer function.
        const auto doubled = [this] (float t)
        {
            const auto closed = [this] (float u) { return 1.0f - shapeCurve.lid (u); };

            return t < 0.5f ? 0.5f * closed (2.0f * t)
                            : 1.0f - 0.5f * closed (2.0f * (1.0f - t));
        };

        juce::Path curve;

        // Dense, because at the extremes the exponent puts almost all of the
        // travel into a few percent of the width and a coarse polyline visibly
        // cuts the corner off.
        constexpr int steps = 512;

        for (int i = 0; i <= steps; ++i)
        {
            const auto t = (float) i / (float) steps;
            const juce::Point<float> point { plot.getX() + t * side,
                                             plot.getBottom() - doubled (t) * side };

            i == 0 ? curve.startNewSubPath (point) : curve.lineTo (point);
        }

        g.setColour (uicolour::accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        g.restoreState();
        paintWordmark (g, bounds);
        return;
    }

    // ---- where the thresholds sit -------------------------------------------
    // Derived here rather than mirrored out of the engine: the engine only
    // refreshes its copy inside processBlock, so in a stopped host these would
    // sit at their defaults until playback started. Run through the engine's own
    // clamp so the drawn floor cannot climb above the drawn ceiling.
    const auto ceilingLin = juce::Decibels::decibelsToGain (
        processor.apvts.getRawParameterValue (ids::ceiling)->load());
    const auto requestedFloorDb = processor.apvts.getRawParameterValue (ids::floorDb)->load();
    const auto floorLin = sidecrush::Engine::clampFloor (
        requestedFloorDb <= floorOffDb ? 0.0f : juce::Decibels::decibelsToGain (requestedFloorDb),
        ceilingLin);

    const auto showThresholds = overlay == Overlay::thresholds;
    const auto centreX = bounds.getCentreX();
    const auto post = processor.apvts.getRawParameterValue (ids::filterPos)->load() > 0.5f;

    // In WTF the two channels stop agreeing -- SPEC 4.5 -- and the display says
    // so: the top of the aperture is the left lid, the bottom is the right, and
    // the output is drawn once per channel at half opacity so the two traces
    // read as one line while they agree and visibly part when they do not.
    const auto wtf = (int) processor.apvts.getRawParameterValue (ids::scLink)->load() == sclink::wtf;

    // Both sidechain gradients run the full +/-1 amplitude span, so their stops
    // can be placed at the ceiling and the floor and stay there as those move.
    // Figma builds these by hand and its annotation asks for "the easiest
    // programmatic way possible" -- which is this, since JUCE will fill a
    // stroke from a gradient just as happily as it fills a shape.
    const auto atAmplitude = [] (float a)
    {
        return juce::jlimit (0.001, 0.999, (1.0 - (double) a) * 0.5);
    };

    // With FLOOR at INSTANT the two floor stops would land on the same spot; a
    // hair of separation keeps the gradient's stops strictly ordered and makes
    // the fade run all the way to the zero crossing, which is what no floor
    // means anyway.
    const auto floorEdge = juce::jmax (floorLin, 0.004f);

    // Solid where the sidechain is above the ceiling, gone where it is below the
    // floor: the trace is drawn only where it is actually doing something.
    juce::ColourGradient window { uicolour::accent, centreX, toY (1.0f),
                                  uicolour::accent, centreX, toY (-1.0f), false };
    window.addColour (atAmplitude (ceilingLin), uicolour::accent);
    window.addColour (atAmplitude (floorEdge), uicolour::accent.withAlpha (0.0f));
    window.addColour (atAmplitude (-floorEdge), uicolour::accent.withAlpha (0.0f));
    window.addColour (atAmplitude (-ceilingLin), uicolour::accent);

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

        // Auto timebase: the design asks for "one or two cycles of the sidechain
        // wave", so the period is measured from the crossing interval and
        // clamped. A 40 Hz sub and a 100 Hz sub then fill the window the same way.
        int64_t window_ = 1024;

        if (previous > 0 && trigger > previous)
            window_ = juce::jlimit<int64_t> (128, 8192, (trigger - previous) * 2);

        const auto startIndex = juce::jmax<int64_t> (1, trigger - window_);
        const auto count = trigger - startIndex;

        if (count >= 8)
        {
            // Two cycles of a 40 Hz sub is ~2400 samples across 380 pixels, so a
            // polyline through every sixth sample is what actually gets drawn --
            // it misses the peaks, and which samples it lands on shifts frame to
            // frame, so the carrier crawls. Each column is drawn as the range of
            // the samples inside it, with each extreme placed at the x of the
            // sample it came from so that diagonals do not turn into staircases.
            const auto columns = juce::jmax (1, (int) bounds.getWidth());
            const auto firstX = bounds.getX() + 0.5f;
            const auto lastX = bounds.getX() + (float) (columns - 1) + 0.5f;

            // Which samples land in one pixel column. The WTF brightness below
            // walks the same ranges, so this is defined once rather than twice
            // -- two copies could disagree about where a column ends and the
            // gradient would then be lit a column out from the trace it tints.
            const auto columnRange = [&] (int column)
            {
                const auto from = startIndex + count * column / columns;
                return std::pair { from, juce::jmax (from + 1,
                                                     startIndex + count * (column + 1) / columns) };
            };

            const auto buildTrace = [&] (auto value)
            {
                juce::Path path;
                auto started = false;

                const auto toX = [&] (int64_t i)
                {
                    return bounds.getX() + (float) (i - startIndex) / (float) count * bounds.getWidth();
                };

                for (int column = 0; column < columns; ++column)
                {
                    const auto [from, to] = columnRange (column);

                    auto low = std::numeric_limits<float>::max();
                    auto high = std::numeric_limits<float>::lowest();
                    auto lowAt = from, highAt = from;

                    for (auto i = from; i < to; ++i)
                    {
                        const auto v = value (fifo.at (i));

                        if (v < low)  { low = v;  lowAt = i; }
                        if (v > high) { high = v; highAt = i; }
                    }

                    const auto firstAt = juce::jmin (lowAt, highAt);
                    const auto secondAt = juce::jmax (lowAt, highAt);
                    const auto first = firstAt == lowAt ? low : high;
                    const auto second = firstAt == lowAt ? high : low;

                    if (! started)
                    {
                        path.startNewSubPath (toX (firstAt), toY (first));
                        started = true;
                    }
                    else
                    {
                        path.lineTo (toX (firstAt), toY (first));
                    }

                    path.lineTo (toX (secondAt), toY (second));
                }

                return path;
            };

            const auto stroke = [] (float thickness)
            {
                return juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded);
            };

            // Closing a trace onto a horizontal edge turns it into a shape. The
            // sidechain crosses its own baseline repeatedly, so the polygon
            // self-intersects -- non-zero winding, which is JUCE's default,
            // fills the lobes either side of it rather than cancelling them.
            const auto closeOnto = [&] (juce::Path path, float y)
            {
                path.lineTo (lastX, y);
                path.lineTo (firstX, y);
                path.closeSubPath();
                return path;
            };

            const auto sidechain = buildTrace ([] (const ScopeFrame& f) { return f.sc; });
            const auto output = buildTrace ([] (const ScopeFrame& f) { return f.out; });
            const auto outputR = wtf ? buildTrace ([] (const ScopeFrame& f) { return f.outR; })
                                     : juce::Path {};

            // WTF draws one output per channel, and the two together have to
            // read as one line where the channels agree and as two where they do
            // not. The right sits at `apart` throughout; the left carries the
            // reading -- full strength where the two are the same signal, as
            // bright as the single trace every other mode draws and with the
            // right hidden underneath it, fading to the same `apart` as they
            // come apart. So brightness is agreement, and a small deviation
            // looks small instead of switching the whole trace to a ghost.
            //
            // The fade is linear up to `fullyApart`, roughly two pixels of
            // separation at this display's scale -- below that the two traces
            // are one line and splitting the ink would only dim it for no
            // reading. It needs a stop per column: the gap turns over at the
            // carrier's rate and not the sidechain's, because inside one half of
            // the sub the lid holds one channel down at its peaks while both run
            // free through the zero crossings. Coarser stops smear every bright
            // stretch away and the whole trace sits at `apart`.
            constexpr auto apart = 0.3f;
            constexpr auto fullyApart = 0.02f;

            const auto strokeOutput = [&] (float alpha)
            {
                const auto line = stroke (1.4f);
                const auto lit = [alpha] (float a) { return uicolour::output.withAlpha (alpha * a); };

                if (! wtf)
                {
                    g.setColour (lit (1.0f));
                    g.strokePath (output, line);
                    return;
                }

                g.setColour (lit (apart));
                g.strokePath (outputR, line);

                // Widest gap inside the column rather than the mean, so that a
                // deviation cannot hide between two stops.
                const auto litAt = [&] (int column)
                {
                    const auto [from, to] = columnRange (column);
                    auto widest = 0.0f;

                    for (auto i = from; i < to; ++i)
                        widest = juce::jmax (widest, std::abs (fifo.at (i).out - fifo.at (i).outR));

                    return lit (juce::jmap (juce::jmin (widest, fullyApart),
                                            0.0f, fullyApart, 1.0f, apart));
                };

                juce::ColourGradient agreement { litAt (0), bounds.getX(), mid,
                                                 litAt (columns - 1), bounds.getRight(), mid, false };

                for (int column = 1; column < columns - 1; ++column)
                    agreement.addColour ((double) column / (double) (columns - 1), litAt (column));

                g.setGradientFill (agreement);
                g.strokePath (output, line);
            };

            // In POST the detector is a rectified envelope, and drawn literally
            // it is a half-wave sitting on the centre line -- which reads as a
            // broken trace rather than as the signal a *symmetric* pair of
            // thresholds is measuring. So its mirror image is drawn alongside
            // it and the envelope brackets the centre the way the lid does.
            // Nothing about the trace itself changes, so it still crosses the
            // ceiling exactly where the lid closes; the reflection is the same
            // sample at the same x, and cannot drift from it.
            juce::Path mirrored;

            if (post)
            {
                mirrored = sidechain;
                mirrored.applyTransform (juce::AffineTransform::verticalFlip (2.0f * mid));
            }

            if (showThresholds)
            {
                // The sidechain is the thing being measured against the bands,
                // so it becomes a solid body and everything else steps back to a
                // ghost. Figma drops the lid aperture entirely here.
                g.setGradientFill (window);
                g.fillPath (closeOnto (sidechain, mid));

                if (post)
                    g.fillPath (closeOnto (mirrored, mid));

                // ... and its outline greys out where it is inside the floor
                // band, where the lid is wide open and the level means nothing.
                juce::ColourGradient outline { uicolour::accent, centreX, toY (1.0f),
                                               uicolour::accent, centreX, toY (-1.0f), false };
                outline.addColour (atAmplitude (floorEdge) - 0.006, uicolour::accent);
                outline.addColour (atAmplitude (floorEdge), uicolour::belowFloor);
                outline.addColour (atAmplitude (-floorEdge), uicolour::belowFloor);
                outline.addColour (atAmplitude (-floorEdge) + 0.006, uicolour::accent);

                g.setGradientFill (outline);
                g.strokePath (sidechain, stroke (1.6f));
                g.strokePath (mirrored, stroke (1.6f));

                strokeOutput (0.1f);
            }
            else
            {
                g.setGradientFill (window);
                g.strokePath (sidechain, stroke (1.6f));
                g.strokePath (mirrored, stroke (1.6f));

                // The lid as the aperture it is: everything outside it masked
                // off, densest against the opening and fading out towards the
                // frame, so the cap visibly closes in from top and bottom.
                g.setGradientFill ({ juce::Colours::white.withAlpha (0.0f), centreX, bounds.getY(),
                                     juce::Colours::white.withAlpha (0.08f), centreX, mid, false });
                g.fillPath (closeOnto (buildTrace ([] (const ScopeFrame& f) { return f.lidTop; }),
                                       bounds.getY()));

                g.setGradientFill ({ juce::Colours::white.withAlpha (0.08f), centreX, mid,
                                     juce::Colours::white.withAlpha (0.0f), centreX, bounds.getBottom(), false });
                // Top edge from the left channel, bottom from the right: in WTF
                // those are the two halves of the split, and under RCTF each is
                // already the edge that half of the sidechain is closing. Every
                // other mode has all four the same number, so this draws the one
                // symmetric aperture it always did.
                g.fillPath (closeOnto (buildTrace ([wtf] (const ScopeFrame& f)
                                                   { return -(wtf ? f.lidBotR : f.lidBot); }),
                                       bounds.getBottom()));

                strokeOutput (1.0f);
            }
        }
    }

    if (showThresholds)
    {
        for (auto sign : { 1.0f, -1.0f })
        {
            const auto edge = sign > 0.0f ? bounds.getY() : bounds.getBottom();
            const auto threshold = toY (sign * ceilingLin);
            const juce::Rectangle<float> band { bounds.getX(), juce::jmin (edge, threshold),
                                                bounds.getWidth(), std::abs (threshold - edge) };

            g.setGradientFill ({ uicolour::accent.withAlpha (0.07f), centreX, edge,
                                 uicolour::accent.withAlpha (0.14f), centreX, threshold, false });
            g.fillRect (band);
        }

        if (floorLin > 0.0f)
        {
            g.setColour (uicolour::clipOn.withAlpha (0.2f));
            g.fillRect (bounds.getX(), toY (floorLin), bounds.getWidth(),
                        toY (-floorLin) - toY (floorLin));
        }
    }

    g.restoreState();
    paintWordmark (g, bounds);
}

//==============================================================================
SettingsPanel::SettingsPanel (SideCrushProcessor& p)
    : scale ("SCALE"),
      link (p, ids::scLink, Pill::Gesture::cycle),
      quality (p, ids::quality, Pill::Gesture::cycle),
      filterPos (p, ids::filterPos, Pill::Gesture::cycle, "FILTER"),
      source (p, ids::scSource, Pill::Gesture::cycle, "SIGNAL"),
      wtfInt (p, ids::wtfInt, Pill::Gesture::drag, "WTF"),
      recti (p, ids::recti, Pill::Gesture::cycle),
      linkWatch (*p.apvts.getParameter (ids::scLink),
                 [this] (float value)
                 {
                     const auto shouldShow = juce::roundToInt (value) == sclink::wtf;

                     if (shouldShow == wtfInt.isVisible())
                         return;

                     wtfInt.setVisible (shouldShow);
                     resized(); // SCALE re-centres on its own when it loses its neighbour
                 })
{
    // The choice names itself, so nothing has to override the text. What the
    // design does say is that the three states are not equals: HQ is the live
    // value, LQ is dimmed to the caption tone so the pair reads as one switch,
    // and YUCK gets its own amber -- a warning, not a reading.
    quality.textColour = [&p]
    {
        switch ((int) p.apvts.getRawParameterValue (ids::quality)->load())
        {
            case 1:  return uicolour::label;
            case 2:  return uicolour::yuck;
            default: return uicolour::value;
        }
    };

    scale.setComponentID ("scale");

    // The parameter is a bool, and "On"/"Off" says nothing about what is on --
    // the two states are two different meanings for the MIX fader, so they are
    // named after those. Whole-label rather than the dim-prefix form its
    // neighbours use, which is how Figma node 2:35 draws it. The click is not
    // the pill's own: it moves MIX as well.
    recti.onClick = [&p] { toggleRecti (p); };

    recti.overrideText = [&p]
    {
        return juce::String (p.apvts.getRawParameterValue (ids::recti)->load() > 0.5f
                                 ? "RECTIFIED" : "CLEAN MIX");
    };

    // A continuous dial has no choice list to right-click, so it gets the three
    // settings that are worth naming -- the two ends and the original behaviour
    // in the middle. Straight from the design's annotation on this pill.
    wtfInt.onRightClick = [this, &p]
    {
        static const juce::StringArray items { "0% - Boring MONO :(",
                                               "50% - L/R independent clipping :3",
                                               "100% - Phase cancellation carnage B)" };

        auto& param = *p.apvts.getParameter (ids::wtfInt);
        const auto current = juce::roundToInt (param.convertFrom0to1 (param.getValue()));

        // The three are 50% apart, so the tick is arithmetic rather than a scan.
        showPillMenu (wtfInt, items, current % 50 == 0 ? current / 50 : -1,
                      [&param] (int choice)
                      {
                          param.beginChangeGesture();
                          param.setValueNotifyingHost (param.convertTo0to1 ((float) (choice * 50)));
                          param.endChangeGesture();
                      });
    };

    for (auto* pill : { &link, &quality, &filterPos, &source, &scale, &wtfInt, &recti })
        addAndMakeVisible (pill);

    linkWatch.sendInitialUpdate();
}

void SettingsPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Figma lifts the whole panel to a lighter slate while it is open.
    paintWell (g, bounds, 12.0f, juce::Colour { 0xff151c22 }, juce::Colour { 0xff2a3844 });
    paintWordmark (g, bounds, uicolour::brandDim.brighter (0.4f));
}

void SettingsPanel::resized()
{
    const auto centreX = getWidth() / 2;
    const auto centreY = getHeight() / 2;

    // One centred row of pills, 21 tall and 8 apart. A null entry is a pill that
    // is not showing -- WTF's, which comes and goes -- and the row closes up
    // around it rather than leaving a hole.
    const auto row = [centreX] (std::initializer_list<std::pair<Pill*, int>> pills, int y)
    {
        auto total = 0;

        for (const auto& [pill, width] : pills)
            if (pill != nullptr)
                total += (total > 0 ? 8 : 0) + width;

        auto x = centreX - total / 2;

        for (const auto& [pill, width] : pills)
            if (pill != nullptr)
            {
                pill->setBounds (x, y, width, 21);
                x += width + 8;
            }
    };

    // Two rows of routing, then a third for the two dials that are not routing at
    // all. WTF joins SCALE there rather than the link it belongs to, because it
    // is an amount and everything on the rows above is a switch.
    //
    // Widths and spacing are read straight off Figma node 2:35: rows 1 and 2 are
    // 29 apart, and the third sits below a wider 41 so the two amounts read as
    // their own group. The block is 91 tall and centred, which is 45 above the
    // panel's middle.
    row ({ { &link, 80 }, { &quality, 43 }, { &recti, 99 } }, centreY - 45);
    row ({ { &filterPos, 101 }, { &source, 107 } }, centreY - 16);
    row ({ { &scale, 113 }, { wtfInt.isVisible() ? &wtfInt : nullptr, 90 } }, centreY + 25);
}

//==============================================================================
SideCrushEditor::SideCrushEditor (SideCrushProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      slopePill (p, ids::slope,   Pill::Gesture::drag),
      floorPill (p, ids::floorDb, Pill::Gesture::drag),
      clipPill  (p, ids::clip,    Pill::Gesture::cycle),
      filterCaption ("FILTER"), shapeCaption ("SHAPE"),
      mixCaption ("MIX", 16.0f),
      gear  (BinaryData::settings_svg, BinaryData::settings_svgSize),
      close (BinaryData::close_svg,    BinaryData::close_svgSize),
      led (p), scope (p), settings (p)
{
    setLookAndFeel (&lookAndFeel);

    addSlider (preSlider,    juce::Slider::LinearVertical,     ids::pre,      {}, true, preAtt);
    addSlider (outputSlider, juce::Slider::LinearVertical,     ids::output,   {}, true, outputAtt);
    addSlider (mixSlider,    juce::Slider::LinearVertical,     ids::mix,      {}, true, mixAtt);
    addSlider (ceilingKnob,  juce::Slider::RotaryVerticalDrag, ids::ceiling,  uicolour::accent, true, ceilingAtt);
    addSlider (filterKnob,   juce::Slider::RotaryVerticalDrag, ids::filterHz, uicolour::accent, false, filterAtt);
    addSlider (shapeKnob,    juce::Slider::RotaryVerticalDrag, ids::shape,    uicolour::accent, false, shapeAtt);

    // The design's dials sweep 270 degrees: 7:30 round to 4:30. JUCE measures
    // clockwise from 12 o'clock and requires both angles to be positive, so the
    // end angle is expressed past a full turn rather than as a negative start.
    for (auto* knob : { &ceilingKnob, &filterKnob, &shapeKnob })
        knob->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                   juce::MathConstants<float>::pi * 2.75f, true);

    const auto filterIsOff = [this]
    {
        return proc.apvts.getRawParameterValue (ids::filterHz)->load() >= filterOffHz - 1.0f;
    };

    // "This shows OFF if the Filter is off."
    slopePill.overrideText = [this, filterIsOff]
    {
        return filterIsOff() ? juce::String ("OFF")
                             : proc.apvts.getParameter (ids::slope)->getCurrentValueAsText();
    };

    // With the filter off there are no slopes to choose between, so a drag that
    // stepped through them would look broken. It brings the filter in instead --
    // downwards, because that is the direction the cutoff moves.
    slopePill.chooseDragTarget = [this, filterIsOff]
    {
        return proc.apvts.getParameter (filterIsOff() ? ids::filterHz : ids::slope);
    };

    slopePill.onClick = [this] { showSlopeMenu(); };

    clipPill.outlined = true;
    clipPill.onTint = uicolour::clipOn;
    clipPill.overrideText = [] { return juce::String ("CLIP"); };

    // Off, CLIP reads as a legend rather than a live value, so the design drops
    // its text to the caption tone.
    clipPill.textColour = [this]
    {
        return proc.apvts.getRawParameterValue (ids::clip)->load() > 0.5f ? uicolour::clipOn
                                                                          : uicolour::label;
    };

    filterCaption.hoverText = [this]
    {
        return proc.apvts.getParameter (ids::filterPos)->getCurrentValueAsText();
    };

    filterCaption.onClick = [this]
    {
        auto& param = *proc.apvts.getParameter (ids::filterPos);
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getValue() > 0.5f ? 0.0f : 1.0f);
        param.endChangeGesture();
    };

    // The MIX caption is a switch as well as a label -- the same one the
    // settings panel carries, because RCTF is a property of this fader and
    // reaching for the gear to change what the fader under your cursor means is
    // a detour.
    //
    // Colour is the mode and nothing else: blue while RCTF is on, grey while it
    // is off, hover included. What hover moves is the word, and it always names
    // the option a click would get you -- so the caption reads as a switch with
    // its far position showing, rather than as a label that lights up.
    const auto rectiOn = [this]
    {
        return proc.apvts.getRawParameterValue (ids::recti)->load() > 0.5f;
    };

    mixCaption.activeText = [rectiOn] { return juce::String (rectiOn() ? "RCTF" : ""); };
    mixCaption.hoverText = [rectiOn] { return juce::String (rectiOn() ? "MIX" : "RCTF"); };
    mixCaption.textColour = [rectiOn] { return rectiOn() ? uicolour::accent : uicolour::label; };
    mixCaption.onClick = [this] { toggleRecti (proc); };

    // The cursor is still on the word when the click lands, and the reveal names
    // the *other* option -- so without a hold the caption would flip straight
    // back to what you just switched away from and look like it had not taken.
    // Two seconds is long enough to read the state you asked for and short
    // enough not to feel stuck.
    mixCaption.clickHoldMs = 2000;

    mixSlider.snapActive = rectiOn;

    // SHAPE's caption only ever reports; letting it take clicks would steal them
    // from the dial whose padded bounds it sits inside.
    shapeCaption.setInterceptsMouseClicks (false, false);

    // ---- who gets to claim the display -------------------------------------
    // Only an actual drag does. The resting state already carries the thresholds
    // in the sidechain's own fade, so raising the bands on hover made them flash
    // every time the pointer crossed a dial on its way somewhere else.
    const auto drag = [this] (bool active) { thresholdDrag = active; updateScopeOverlay(); };

    ceilingKnob.onDragStart = [drag] { drag (true); };
    ceilingKnob.onDragEnd = [drag] { drag (false); };
    floorPill.onDragActive = drag;
    scope.onDragActive = drag;

    filterKnob.onDragStart = [this] { filterCaption.setValueText (filterKnob.getTextFromValue (filterKnob.getValue())); };
    filterKnob.onDragEnd = [this] { filterCaption.setValueText ({}); };

    filterKnob.onValueChange = [this]
    {
        if (filterCaption.isShowingValue())
            filterCaption.setValueText (filterKnob.getTextFromValue (filterKnob.getValue()));
    };

    // "While hovering and dragging the Display screen should show ramp as it
    // would be applied." The audio traces stand down entirely for it -- a
    // transfer curve and a waveform share an axis and mean different things by it.
    shapeKnob.onDragStart = [this]
    {
        shapeCaption.setValueText (shapeKnob.getTextFromValue (shapeKnob.getValue()));
        shapeDrag = true;
        updateScopeOverlay();
    };

    shapeKnob.onDragEnd = [this]
    {
        shapeCaption.setValueText ({});
        shapeDrag = false;
        updateScopeOverlay();
    };

    shapeKnob.onValueChange = [this]
    {
        if (shapeCaption.isShowingValue())
            shapeCaption.setValueText (shapeKnob.getTextFromValue (shapeKnob.getValue()));

        if (shapeDrag)
            scope.repaint();
    };

    // ---- the scale switch, which is a preference and not a parameter --------
    settings.scale.overrideText = [this]
    {
        return juce::String (juce::roundToInt (scaleFactor * 100.0f)) + "%";
    };

    settings.scale.onClick = [this]
    {
        auto index = 0;

        for (int i = 0; i < (int) std::size (scaleSteps); ++i)
            if (juce::approximatelyEqual (scaleFactor, scaleSteps[i]))
                index = i;

        setScale (scaleSteps[(index + 1) % (int) std::size (scaleSteps)]);
        settings.scale.repaint();
    };

    // Right-click lists the choices -- see Pill::showChoiceMenu. These two are
    // the pills that need more than the parameter can say: SLOPE's menu also
    // brings the filter in, and SCALE has no parameter at all.
    slopePill.onRightClick = [this] { showSlopeMenu(); };
    settings.scale.onRightClick = [this] { showScaleMenu(); };

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

    // After the dials, so they win the clicks inside the dials' padded bounds.
    addAndMakeVisible (filterCaption);
    addAndMakeVisible (shapeCaption);
    addAndMakeVisible (mixCaption);

    gear.onClick = [this] { showSettings (true); };
    close.onClick = [this] { showSettings (false); };

    lastFilterPost = proc.filterIsPost.load (std::memory_order_relaxed);
    lastRecti = proc.rectiIsOn.load (std::memory_order_relaxed);

    setScale (scalePref->get());

    // One timer for both animated children. 30 is plenty for a scope and costs
    // half of 60; it runs for as long as the editor is open, whether or not the
    // host is playing anything.
    startTimerHz (30);
}

SideCrushEditor::~SideCrushEditor()
{
    setLookAndFeel (nullptr);
}

void SideCrushEditor::showSlopeMenu()
{
    auto& slope = *proc.apvts.getParameter (ids::slope);
    const auto choices = slope.getAllValueStrings();

    showPillMenu (slopePill, choices,
                  juce::roundToInt (slope.getValue() * (float) (choices.size() - 1)),
                  [this] (int index) { applySlope (index); });
}

void SideCrushEditor::showScaleMenu()
{
    juce::StringArray items;
    auto current = 0;

    for (int i = 0; i < (int) std::size (scaleSteps); ++i)
    {
        items.add (juce::String (juce::roundToInt (scaleSteps[i] * 100.0f)) + "%");

        if (juce::approximatelyEqual (scaleFactor, scaleSteps[i]))
            current = i;
    }

    showPillMenu (settings.scale, items, current,
                  [this] (int index)
                  {
                      setScale (scaleSteps[index]);
                      settings.scale.repaint();
                  });
}

void SideCrushEditor::applySlope (int index)
{
    auto& slope = *proc.apvts.getParameter (ids::slope);
    slope.beginChangeGesture();
    slope.setValueNotifyingHost (slope.convertTo0to1 ((float) index));
    slope.endChangeGesture();

    // Picking a slope for a filter that is switched off asks for a filter. 160 Hz
    // is low enough to be doing something to a sub without swallowing it.
    if (proc.apvts.getRawParameterValue (ids::filterHz)->load() < filterOffHz - 1.0f)
        return;

    auto& filter = *proc.apvts.getParameter (ids::filterHz);
    filter.beginChangeGesture();
    filter.setValueNotifyingHost (filter.convertTo0to1 (160.0f));
    filter.endChangeGesture();
}

void SideCrushEditor::updateScopeOverlay()
{
    scope.setOverlay (shapeDrag     ? ScopeComponent::Overlay::shape
                    : thresholdDrag ? ScopeComponent::Overlay::thresholds
                                    : ScopeComponent::Overlay::traces);

    // CLIP belongs to the scope's frame, not to the SHAPE curve that stands in
    // for it -- only the wordmark and the gear carry over.
    clipPill.setVisible (! settings.isVisible() && ! shapeDrag);
}

void SideCrushEditor::addSlider (juce::Slider& slider, juce::Slider::SliderStyle style,
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

void SideCrushEditor::setScale (float scale)
{
    scaleFactor = scale;
    scalePref->set (scale);

    setTransform (juce::AffineTransform::scale (scale));
    setSize (designWidth, designHeight);
}

void SideCrushEditor::showSettings (bool shouldShow)
{
    scope.setVisible (! shouldShow);
    settings.setVisible (shouldShow);
    gear.setVisible (! shouldShow);
    close.setVisible (shouldShow);
    updateScopeOverlay();
}

void SideCrushEditor::refreshFromParameters()
{
    for (juce::Slider* slider : std::initializer_list<juce::Slider*> {
             &preSlider, &outputSlider, &mixSlider, &ceilingKnob, &filterKnob, &shapeKnob })
        slider->updateText();

    updateScopeOverlay();
    timerCallback();
    repaint();
}

void SideCrushEditor::timerCallback()
{
    // Another instance's editor may have moved the shared scale. Polling the
    // timer that is already running costs one compare a frame and needs no
    // listener to register, unregister, or outlive.
    if (const auto scale = scalePref->get(); ! juce::approximatelyEqual (scale, scaleFactor))
    {
        setScale (scale);
        settings.scale.repaint();
    }

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

    // MIX's readout names its two components under RCTF and reads a plain
    // percentage otherwise, so the mode flipping is what rebuilds it. Written
    // from here as well as from the audio thread, for the reason above: same
    // value either way, and this path still works in a host that is idle.
    if (const auto recti = proc.apvts.getRawParameterValue (ids::recti)->load() > 0.5f;
        recti != lastRecti)
    {
        lastRecti = recti;
        proc.rectiIsOn.store (recti, std::memory_order_relaxed);
        mixSlider.updateText();
        mixCaption.repaint();
    }

    // The FLOOR readout brackets its value while the ceiling is holding it down,
    // so the ceiling moving is what repaints it -- nothing else would.
    if (const auto ceiling = proc.apvts.getRawParameterValue (ids::ceiling)->load();
        ! juce::approximatelyEqual (ceiling, lastCeilingDb))
    {
        lastCeilingDb = ceiling;
        floorPill.repaint();
    }

    // The dial's pointer goes flat when its filter is switched off -- the design
    // calls it out as the only dial that does this.
    const auto filterOff = proc.apvts.getRawParameterValue (ids::filterHz)->load() >= filterOffHz - 1.0f;
    const auto wanted = filterOff ? uicolour::idle : uicolour::accent;

    if (filterKnob.findColour (juce::Slider::rotarySliderFillColourId) != wanted)
    {
        filterKnob.setColour (juce::Slider::rotarySliderFillColourId, wanted);
        slopePill.repaint();
    }
}

void SideCrushEditor::paint (juce::Graphics& g)
{
    g.fillAll (uicolour::background);

    // `inset 8px 10px 22.2px rgba(255,255,255,0.02)` -- a barely-there lift in
    // the top-left corner, but it is measurably there in the reference render.
    g.setGradientFill ({ juce::Colours::white.withAlpha (0.021f), 0.0f, 0.0f,
                         juce::Colours::transparentWhite, 260.0f, 200.0f, true });
    g.fillRect (getLocalBounds());

    // The scope's `0 0 180px rgba(78,228,251,0.05)`. A real 180px blur here
    // would cost far more than the whole rest of this paint, and the result is
    // a soft round falloff either way.
    const juce::Rectangle<float> scopeArea { 451.0f, 48.0f, 380.0f, 230.0f };
    g.setGradientFill ({ uicolour::accent.withAlpha (0.055f), scopeArea.getCentre(),
                         juce::Colours::transparentBlack,
                         scopeArea.getCentre().translated (0.0f, scopeArea.getHeight() * 1.6f), true });
    g.fillRect (scopeArea.expanded (110.0f));

    g.setFont (uiFont (16.0f));
    g.setColour (uicolour::label);
    g.drawText ("PRE",     juce::Rectangle<int> {  48, 48, 42, 19 }, juce::Justification::centred);
    g.drawText ("CEILING", juce::Rectangle<int> { 164, 48, 77, 19 }, juce::Justification::centred);
    g.drawText ("OUT",     juce::Rectangle<int> { 878, 48, 42, 19 }, juce::Justification::centred);
}

void SideCrushEditor::resized()
{
    // Every number here is read straight off Figma node 1:11 at 1:1. Dials are
    // given SideCrushLookAndFeel::knobMargin of padding on every side so their
    // drop shadow and the pointer's glow are not clipped by their own bounds.
    preSlider.setBounds    (  48,  83,  42, 192); // track 83..243, readout to 275
    outputSlider.setBounds ( 878,  83,  42, 192);
    mixSlider.setBounds    ( 966,  83,  42, 192); // outermost: it is the one with
                                                  // a switch above it, so it wants
                                                  // the edge rather than a neighbour

    ceilingKnob.setBounds  ( 113,  69, 188, 206); // r 80 at (207,163)
    filterKnob.setBounds   ( 320,  66,  78,  78); // r 25 at (359,105)
    shapeKnob.setBounds    ( 320, 181,  78,  78); // r 25 at (359,220)

    led.setBounds          ( 224,  33,  48,  48); // 4px dot at (248,57), rest is glow

    // Wider than the design's 48, because these also have to hold a value like
    // "12.50 kHz" once their dial is moving. Both stay centred on their dial.
    filterCaption.setBounds ( 325, 137,  68, 13);
    shapeCaption.setBounds  ( 324, 176,  70, 13);

    // Wider than the design's 42, for the reason the two captions above are:
    // Zalando Sans Expanded puts RCTF over that at 16pt and it comes back
    // ellipsised. Still centred on the fader, and it has the panel's edge to
    // grow into now that MIX is the outer of the two.
    mixCaption.setBounds    ( 954,  48,  66, 19);

    slopePill.setBounds    ( 313,  48,  92,  21);
    floorPill.setBounds    ( 313, 257,  92,  21);

    scope.setBounds        ( 451,  48, 380, 230);
    settings.setBounds     ( 451,  48, 380, 230);

    gear.setBounds         ( 805,  58,  16,  16);
    close.setBounds        ( 805,  58,  16,  16);
    clipPill.setBounds     ( 768, 247,  53,  21);
}
