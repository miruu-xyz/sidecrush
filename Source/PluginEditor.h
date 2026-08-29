#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

//==============================================================================
// Colours are taken from a 1:1 render of Figma node 1:11, sampled pixel by
// pixel (Resources/reference/figma-1-11.png, tools/pngpick.py) rather than read
// off the layer list. That matters because most of the design's text and lines
// are drawn with `mix-blend-mode: color-dodge`, which has no JUCE equivalent --
// #b1b1b1 dodged over the #101419 background lands at #344151, and only the
// second number is any use here. Sampling bakes the blend in once.
namespace hccolour
{
    const juce::Colour background  { 0xff101419 }; // page
    const juce::Colour wellCentre  { 0xff090c10 }; // pill / scope interior, middle
    const juce::Colour wellEdge    { 0xff101419 }; // ... and at its edges
    const juce::Colour scopeCentre { 0xff080a0d };
    const juce::Colour scopeEdge   { 0xff0d1014 };

    const juce::Colour knobTop     { 0xff2a333d }; // knob body gradient
    const juce::Colour knobBottom  { 0xff181d23 };
    const juce::Colour thumbTop    { 0xff404c5e }; // fader cap gradient
    const juce::Colour thumbBottom { 0xff2c3440 };
    const juce::Colour track       { 0xff192028 };

    const juce::Colour label       { 0xff344151 }; // section captions, dodged
    const juce::Colour value       { 0xffe4e7ea }; // readouts, no blend
    const juce::Colour hairline    { 0xff1d242d }; // dodged #b1b1b1 inside the scope
    const juce::Colour brand       { 0xff7891b7 };
    // "by miruu" is #717f8f dodged in Figma, so its rendered value depends on what
    // is behind it. Baked against the lower lid band, which is what it sits on
    // once the threshold overlays are always drawn.
    const juce::Colour brandDim    { 0xff1e3842 };

    const juce::Colour accent      { 0xff4ee4fb }; // cyan: sidechain, live values
    const juce::Colour idle        { 0xff3a4552 }; // a dial whose parameter is off
    const juce::Colour clipOn      { 0xfff85050 };
    const juce::Colour clipBorder  { 0xffe73131 };
    const juce::Colour scopeLine   { 0xff202932 }; // the scope's zero line
    const juce::Colour output      { 0xffdfdfdf }; // the post-lid trace
}

//==============================================================================
// Zalando Sans Expanded, embedded. Figma's sizes are em sizes, so they have to
// go through withPointHeight -- withHeight would set ascent+descent instead and
// come out visibly too small.
juce::Font hcFont (float pointHeight);

// The recessed slot behind every pill and behind the scope: a radial darkening
// of the page colour plus a top-left inner shadow. Figma draws it as a 50%
// black radial gradient over the parent, which is the same thing.
void paintWell (juce::Graphics&, juce::Rectangle<float>, float corner,
                juce::Colour centre, juce::Colour edge);

//==============================================================================
class HardCapLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    HardCapLookAndFeel();

    // Knob and fader geometry both come from the design's own proportions: the
    // pointer runs from 0.386R to R, and the fader travels the full track.
    static constexpr float knobMargin = 14.0f; // room for the pointer's glow

    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h,
                           float pos, float min, float max,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    juce::Label* createSliderTextBox (juce::Slider&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
};

//==============================================================================
// Everything the design calls "Generic Interactable": a 21px-tall well with
// centred text that lights up cyan on hover. Two behaviours share it -- drag to
// change a continuous or stepped value, or click to cycle a choice -- because
// the design draws them identically and only the gesture differs.
class Pill final : public juce::Component
{
public:
    enum class Gesture { drag, cycle };

    Pill (HardCapProcessor&, const char* paramId, Gesture,
          juce::String dimPrefix = {});

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Set by the editor for the slope pill, which the design blanks to OFF when
    // the filter it belongs to is off.
    std::function<juce::String()> overrideText;

    void setOutlined (bool shouldOutline) { outlined = shouldOutline; }

    // CLIP is the only control that recolours itself when engaged. Left clear,
    // a pill keeps the same look in both states, which is what the settings
    // switches do.
    juce::Colour onTint { 0x00000000 };

private:
    juce::RangedAudioParameter& param;

    // Not an APVTS::Listener: a host automating a parameter calls that straight
    // from the audio thread, and marshalling the repaint with
    // MessageManager::callAsync allocates a message every single time.
    // ParameterAttachment is an AsyncUpdater underneath, which allocates once.
    juce::ParameterAttachment attachment;

    const Gesture gesture;
    const juce::String prefix;

    bool outlined = false; // CLIP draws a permanent border; the others do not
    bool hovered = false;
    bool dragging = false;
    float valueAtDragStart = 0.0f;
};

//==============================================================================
// The word under the FILTER dial. Reads "FILTER" at rest, and swaps to a cyan
// PRE / POST while the pointer is over it; clicking flips the two.
class FilterLabel final : public juce::Component
{
public:
    explicit FilterLabel (HardCapProcessor&);

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    HardCapProcessor& proc;
    bool hovered = false;
};

//==============================================================================
// The gear, and the close cross that replaces it while the panel is open. Both
// are the design's own exported vectors -- redrawing a gear by hand never lands
// on the same glyph.
class IconButton final : public juce::Component
{
public:
    IconButton (const void* svgData, int svgSize);

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void()> onClick;

private:
    // Recoloured once per state rather than per paint -- replaceColour has to
    // copy the whole Drawable tree.
    std::unique_ptr<juce::Drawable> resting, lit;
    bool hovered = false;
};

//==============================================================================
// Instantaneous gain reduction, as the dot beside the CEILING caption.
class ActivityLed final : public juce::Component
{
public:
    explicit ActivityLed (HardCapProcessor&);

    void paint (juce::Graphics&) override;
    void refresh();

private:
    HardCapProcessor& processor;
    float level = 0.0f;
};

//==============================================================================
// Cyan filtered sidechain, the white output squashing against a grey aperture,
// a red floor band and cyan lid bands. SPEC 5.1.
class ScopeComponent final : public juce::Component
{
public:
    explicit ScopeComponent (HardCapProcessor&);

    void paint (juce::Graphics&) override;
    void refresh();

    // The SHAPE dial asks the display to preview the curve it is about to
    // apply, so the editor tells the scope when that dial is being touched.
    void setShapePreview (bool shouldShow);

private:
    HardCapProcessor& processor;
    int64_t snapshotHead = 0;
    bool shapePreview = false;
};

//==============================================================================
// The four routing switches. Figma has no popup for the gear; the settings are
// drawn as a variant of the scope itself, so this takes the scope's place at
// the same bounds rather than floating above it.
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (HardCapProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Pill link, hq, filterPos, source;
};

//==============================================================================
class HardCapEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    // The design's own canvas. Everything is laid out against these numbers and
    // the whole editor is scaled as one, so they never change.
    static constexpr int designWidth = 968;
    static constexpr int designHeight = 326;

    explicit HardCapEditor (HardCapProcessor&);
    ~HardCapEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    // Both are also what tools/shot.cpp drives to render a state that would
    // otherwise need a mouse or a running message loop.
    void showSettings (bool shouldShow);
    void refreshFromParameters();

private:
    void timerCallback() override;
    void setScale (float);

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void addSlider (juce::Slider&, juce::Slider::SliderStyle, const char* paramId,
                    juce::Colour pointer, bool withReadout,
                    std::unique_ptr<SliderAttachment>&);

    HardCapProcessor& proc;
    HardCapLookAndFeel lookAndFeel;

    juce::Slider preSlider, outputSlider, ceilingKnob, filterKnob, shapeKnob;
    std::unique_ptr<SliderAttachment> preAtt, outputAtt, ceilingAtt, filterAtt, shapeAtt;

    Pill slopePill, floorPill, clipPill;
    FilterLabel filterLabel;
    IconButton gear, close;
    ActivityLed led;
    ScopeComponent scope;
    SettingsPanel settings;

    float scaleFactor = 1.0f;

    // The FILTER readout relabels itself in POST, and nothing else repaints it.
    bool lastFilterPost = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardCapEditor)
};
