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
namespace uicolour
{
    const juce::Colour background  { 0xff101419 }; // page, and every well's edge
    const juce::Colour wellCentre  { 0xff090c10 }; // pill / scope interior, middle
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
    const juce::Colour output      { 0xffdfdfdf }; // the post-lid trace
    const juce::Colour yuck        { 0xff8d5c3d }; // the one warning tone here
    const juce::Colour belowFloor  { 0xff9e9e9e }; // sidechain where it is under
                                                   // the floor, doing nothing
}

//==============================================================================
// Zalando Sans Expanded, embedded. Figma's sizes are em sizes, so they have to
// go through withPointHeight -- withHeight would set ascent+descent instead and
// come out visibly too small.
juce::Font uiFont (float pointHeight);

// The recessed slot behind every pill and behind the scope: a radial darkening
// of the page colour plus a top-left inner shadow. Figma draws it as a 50%
// black radial gradient over the parent, which is the same thing.
void paintWell (juce::Graphics&, juce::Rectangle<float>, float corner,
                juce::Colour centre, juce::Colour edge);

// "SIDECRUSH by miruu", bottom-left of whichever panel is showing. The dim half is
// #717f8f dodged in Figma, so its rendered value depends on what is behind it
// and the caller says which panel it is on.
void paintWordmark (juce::Graphics&, juce::Rectangle<float> panel,
                    juce::Colour dim = uicolour::brandDim);

//==============================================================================
// The UI scale is one preference for the whole plug-in rather than one per
// instance: it is a property of the screen it is being read on, not of the
// session, so it belongs in the user's settings file and not in the saved
// state. Every editor in the process shares this one object -- held through
// juce::SharedResourcePointer, so it exists for exactly as long as some editor
// does -- and each polls it on its own timer, which is how a change made in one
// window reaches the others.
//
// Two processes open at once will not see each other's change: the file is read
// when the first editor in a process opens it. The next one to launch picks it
// up.
struct ScalePreference
{
    ScalePreference();

    float get() const;
    void set (float scale);

    std::unique_ptr<juce::PropertiesFile> file;
};

//==============================================================================
class SideCrushLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SideCrushLookAndFeel();

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
// centred text that lights up cyan on hover. Several behaviours share it --
// drag a value, click to cycle a choice, click to open a menu -- because the
// design draws them identically and only the gesture differs.
class Pill final : public juce::Component
{
public:
    enum class Gesture { drag, cycle };

    Pill (SideCrushProcessor&, const char* paramId, Gesture, juce::String dimPrefix = {});

    // Not backed by a parameter. The UI scale is a preference, and putting it in
    // the plug-in's automation list would be lying about what it is. Click-driven
    // by definition: with nothing to read there is nothing to drag.
    explicit Pill (juce::String dimPrefix);

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Replaces whatever the parameter would say -- the slope selector reading
    // OFF, the scale switch having no parameter to ask.
    std::function<juce::String()> overrideText;

    // Set to take over the click: the slope selector opens a menu instead of
    // stepping blindly through eight values.
    std::function<void()> onClick;

    // Set to take over the right-click, whose default is to list the parameter's
    // own choices. The two pills that override it are the ones a plain list would
    // get wrong: SLOPE, which also has to switch the filter on, and SCALE, which
    // has no parameter to list.
    std::function<void()> onRightClick;

    std::function<void (bool)> onDragActive;

    // Decided at mouse-down, because a pill can act on something other than the
    // parameter it displays: with the filter off there are no slopes to choose
    // between, so dragging the slope selector moves the filter instead.
    std::function<juce::RangedAudioParameter*()> chooseDragTarget;

    bool outlined = false; // CLIP draws a permanent border; the others do not

    // CLIP is the only control that recolours itself when engaged. Left clear,
    // a pill keeps the same look in both states.
    juce::Colour onTint { 0x00000000 };

    // How the pill's text is coloured, when the plain value tone is not it:
    // QUALITY gives each of its three states its own, and CLIP dims to the
    // caption tone when off. Left unset, a pill reads in the value tone.
    std::function<juce::Colour()> textColour;

private:
    void drawLabel (juce::Graphics&, juce::String text, juce::Colour);

    // Right-click on anything backed by a list of choices: the whole list at
    // once, so a three-way switch does not have to be cycled to be read.
    void showChoiceMenu();

    juce::RangedAudioParameter* param = nullptr;

    // Not an APVTS::Listener: a host automating a parameter calls that straight
    // from the audio thread, and marshalling the repaint with
    // MessageManager::callAsync allocates a message every single time.
    // ParameterAttachment is an AsyncUpdater underneath, which allocates once.
    std::unique_ptr<juce::ParameterAttachment> attachment;

    const Gesture gesture;
    const juce::String prefix;

    bool hovered = false;

    juce::RangedAudioParameter* dragTarget = nullptr; // non-null while dragging
    float valueAtDragStart = 0.0f;
};

//==============================================================================
// The word under a dial. While that dial is being dragged the caption gives way
// to its value, so the pointer's position always has a number attached to it.
// FILTER additionally reads a cyan PRE / POST on hover and flips the two when
// clicked, which is the design's "Variant2".
class DialCaption final : public juce::Component
{
public:
    explicit DialCaption (juce::String captionText, float fontHeight = 12.0f);

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    void setValueText (juce::String);
    bool isShowingValue() const noexcept { return valueText.isNotEmpty(); }

    std::function<juce::String()> hoverText; // drawn in accent, if set
    std::function<void()> onClick;

    // Set when the caption names a mode that is currently engaged: it stands in
    // for the caption and reads in the accent tone, so the mode is legible
    // without hovering. CLIP recolours itself for the same reason.
    std::function<juce::String()> activeText;

private:
    const juce::String caption;
    const float fontSize;
    juce::String valueText;
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
    explicit ActivityLed (SideCrushProcessor&);

    void paint (juce::Graphics&) override;
    void refresh();

private:
    SideCrushProcessor& processor;
    float level = 0.0f;
};

//==============================================================================
// Cyan filtered sidechain, the white output squashing against a grey aperture,
// and the threshold bands. SPEC 5.1.
//
// The overlays are not decoration and are not all on at once: each one answers
// the question the control being touched is asking, and hides whatever would
// compete with the answer.
class ScopeComponent final : public juce::Component
{
public:
    enum class Overlay
    {
        traces,     // at rest: the lid aperture, the sidechain and the output.
                    // The sidechain's own fade already says where the thresholds
                    // are, so nothing has to be overlaid to make this readable
        thresholds, // *dragging* a threshold: the sidechain fills out and
                    // everything else gets out of its way, against the bands
        shape       // dragging SHAPE: the curve alone, no audio at all
    };

    explicit ScopeComponent (SideCrushProcessor&);

    void paint (juce::Graphics&) override;
    void refresh();

    void setOverlay (Overlay);

    // The display's vertical axis is amplitude, and so is CEILING once it is
    // tapered that way, so the scope is a perfectly good CEILING control: drag
    // anywhere in it and the threshold -- and the dial -- follow.
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void (bool)> onDragActive;

private:
    SideCrushProcessor& processor;
    int64_t snapshotHead = 0;
    Overlay overlay = Overlay::traces;

    bool dragging = false;
    float ceilingAtDragStart = 0.0f;

    // The DSP's own lookup table, not a second copy of the formula. The curve on
    // screen is then the curve the audio takes, clamp and quantisation included.
    sidecrush::ShapeTable shapeCurve;
};

//==============================================================================
// The routing switches. Figma has no popup for the gear; the settings are drawn
// as a variant of the scope itself, so this takes the scope's place at the same
// bounds rather than floating above it.
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (SideCrushProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Wired by the editor. Sits apart from the routing switches because it
    // changes nothing about the audio.
    Pill scale;

private:
    Pill link, quality, filterPos, source, wtfInt, recti;

    // WTF's intensity only exists while WTF is selected -- Figma's annotation on
    // the pill says so, and a dial that does nothing is worse than no dial. It
    // is hidden rather than dimmed because it shares its row with SCALE, which
    // simply re-centres; the routing rows above it never reflow.
    //
    // A ParameterAttachment rather than the editor's timer, for the reason the
    // Pill gives: the host can move SC LINK from the audio thread, and this is
    // the one mechanism that marshals it without allocating per change.
    juce::ParameterAttachment linkWatch;
};

//==============================================================================
// MIX's midpoint is the whole rectified signal, and under RECTI it is the only
// point on the fader with a name rather than a proportion -- so it catches
// under the cursor. Off, and anywhere but right beside it, the fader is
// ordinary: this is a detent, not a step.
class SnappingSlider final : public juce::Slider
{
public:
    std::function<bool()> snapActive;

    double snapValue (double attempted, DragMode mode) override
    {
        if (mode == notDragging || snapActive == nullptr || ! snapActive())
            return attempted;

        return std::abs (attempted - 50.0) <= 2.5 ? 50.0 : attempted;
    }
};

//==============================================================================
class SideCrushEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    // The design's own canvas. Everything is laid out against these numbers and
    // the whole editor is scaled as one, so they never change.
    static constexpr int designWidth = 1056;
    static constexpr int designHeight = 326;

    explicit SideCrushEditor (SideCrushProcessor&);
    ~SideCrushEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Both are also what tools/shot.cpp drives to render a state that would
    // otherwise need a mouse or a running message loop.
    void showSettings (bool shouldShow);
    void refreshFromParameters();

private:
    void timerCallback() override;
    void setScale (float);
    void updateScopeOverlay();
    void showSlopeMenu();
    void applySlope (int index);
    void showScaleMenu();

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void addSlider (juce::Slider&, juce::Slider::SliderStyle, const char* paramId,
                    juce::Colour pointer, bool withReadout,
                    std::unique_ptr<SliderAttachment>&);

    SideCrushProcessor& proc;
    SideCrushLookAndFeel lookAndFeel;

    juce::Slider preSlider, outputSlider, ceilingKnob, filterKnob, shapeKnob;
    SnappingSlider mixSlider;
    std::unique_ptr<SliderAttachment> preAtt, outputAtt, mixAtt, ceilingAtt, filterAtt, shapeAtt;

    Pill slopePill, floorPill, clipPill;
    DialCaption filterCaption, shapeCaption, mixCaption;
    IconButton gear, close;
    ActivityLed led;
    ScopeComponent scope;
    SettingsPanel settings;

    // Which controls are currently claiming the display. Kept as flags rather
    // than queried from the mouse so the headless renderer can set them.
    bool thresholdDrag = false;
    bool shapeDrag = false;

    // Shared with every other open editor, and outlives all of them.
    juce::SharedResourcePointer<ScalePreference> scalePref;
    float scaleFactor = 1.0f;

    // The FILTER readout relabels itself in POST, and nothing else repaints it.
    bool lastFilterPost = false;

    // MIX's readout names its two components under RECTI and reads a plain
    // percentage otherwise, so flipping the mode is what rebuilds its text --
    // the same arrangement, for the same reason, as the flag above it.
    bool lastRecti = false;

    // The FLOOR readout brackets its value once the ceiling is holding it down,
    // so it repaints when the *ceiling* moves. Starts outside the range, so the
    // first tick always syncs.
    float lastCeilingDb = 1.0e9f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SideCrushEditor)
};
