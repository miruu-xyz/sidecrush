#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

//==============================================================================
namespace hccolour
{
    const juce::Colour background { 0xff12171d };
    const juce::Colour panel      { 0xff171e26 };
    const juce::Colour line       { 0xff252e39 };
    const juce::Colour text       { 0xffe4e7ea };
    const juce::Colour dim        { 0xff8d959d };
    const juce::Colour faint      { 0xff636971 };
    const juce::Colour accent     { 0xff4ee4fb };
    const juce::Colour ceiling    { 0xff5b8cff };
    const juce::Colour floorLine  { 0xfff85050 };
}

//==============================================================================
class HardCapLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    HardCapLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h,
                           float pos, float min, float max,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool isDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override;
};

//==============================================================================
// Cyan filtered sidechain, blue/red threshold lines, a grey aperture that
// closes in from top and bottom, and the white output squashing against it.
// SPEC 5.1.
class ScopeComponent final : public juce::Component,
                             private juce::Timer
{
public:
    explicit ScopeComponent (HardCapProcessor&);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    HardCapProcessor& processor;
    int64_t snapshotHead = 0;
};

//==============================================================================
class HardCapEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HardCapEditor (HardCapProcessor&);
    ~HardCapEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    juce::Slider& addSlider (juce::Slider::SliderStyle, const juce::String& paramId,
                             std::unique_ptr<SliderAttachment>&);
    juce::ComboBox& addCombo (const juce::String& paramId,
                              std::unique_ptr<ComboAttachment>&);

    HardCapProcessor& proc;
    HardCapLookAndFeel lookAndFeel;
    ScopeComponent scope;

    juce::Slider preSlider, ceilingKnob, filterKnob, shapeKnob, floorField, outputSlider;
    juce::ComboBox slopeBox, filterPosBox, scLinkBox, scSourceBox;
    juce::TextButton clipButton { "CLIP" };

    std::unique_ptr<SliderAttachment> preAtt, ceilingAtt, filterAtt, shapeAtt, floorAtt, outputAtt;
    std::unique_ptr<ComboAttachment> slopeAtt, filterPosAtt, scLinkAtt, scSourceAtt;
    std::unique_ptr<ButtonAttachment> clipAtt;

    // The LED beside CEILING tracks instantaneous gain reduction (SPEC 5.2).
    class ActivityLed final : public juce::Component, private juce::Timer
    {
    public:
        explicit ActivityLed (HardCapProcessor&);
        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;

        HardCapProcessor& processor;
        float level = 0.0f;
    };

    ActivityLed led;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardCapEditor)
};
