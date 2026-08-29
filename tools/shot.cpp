// Renders the editor to a PNG without a host and without opening any audio
// device. See the target comment in CMakeLists.txt for why this exists.
//
//   hardcap_shot out.png [<param>=<value>] [hover=<id>] [settings]
//                        [audio=<sidechain Hz>] [drag=<id>[:<dy>]] [scale=N]
//                        [crop=x,y,w,h]
//
// <param> is any id from ids::, given in the parameter's own units, e.g.
// ceiling=-24, slope=3, clip=0. Unknown keys are an error rather than a silent
// no-op -- a typo that quietly renders the default state is worse than useless
// when the whole point is comparing against a design.

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
    juce::Component* findById (juce::Component& parent, const juce::String& id)
    {
        for (auto* child : parent.getChildren())
        {
            if (child->getComponentID() == id)
                return child;

            if (auto* found = findById (*child, id))
                return found;
        }

        return nullptr;
    }

    // A Component only learns about the mouse from a real one, which a headless
    // render does not have. These build the event JUCE would have delivered, so
    // the component takes exactly the path it takes in a host.
    juce::MouseEvent eventAt (juce::Component& c, juce::Point<float> position,
                              juce::Point<float> downAt, bool dragged)
    {
        return { juce::Desktop::getInstance().getMainMouseSource(),
                 position, juce::ModifierKeys::currentModifiers,
                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                 &c, &c,
                 juce::Time::getCurrentTime(), downAt,
                 juce::Time::getCurrentTime(), 1, dragged };
    }

    void setHovered (juce::Component& c, bool on)
    {
        const auto centre = c.getLocalBounds().getCentre().toFloat();
        const auto e = eventAt (c, centre, centre, false);

        on ? c.mouseEnter (e) : c.mouseExit (e);
    }

    // A real press and vertical drag, so what a drag actually *does* to a value
    // can be checked, not just what it does to the display. The button stays
    // down: the point is to render the state mid-gesture. releaseDrag lifts it
    // again once the snapshot is taken -- a parameter whose beginChangeGesture
    // is never matched asserts when it is destroyed, and that noise would hide
    // a real assertion later.
    void dragBy (juce::Component& c, float dy)
    {
        const auto centre = c.getLocalBounds().getCentre().toFloat();

        c.mouseDown (eventAt (c, centre, centre, false));
        c.mouseDrag (eventAt (c, centre.translated (0.0f, dy), centre, true));
    }

    void releaseDrag (juce::Component& c, float dy)
    {
        const auto centre = c.getLocalBounds().getCentre().toFloat();
        c.mouseUp (eventAt (c, centre.translated (0.0f, dy), centre, true));
    }
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::puts ("usage: hardcap_shot out.png [param=value ...] [hover=id] "
                   "[settings] [audio=hz] [drag=id] [scale=N] [crop=x,y,w,h]");
        return 1;
    }

    const juce::ScopedJuceInitialiser_GUI gui;

    HardCapProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto editor = std::make_unique<HardCapEditor> (processor);

    juce::String hoverId;
    juce::Rectangle<int> crop;
    auto scale = 1.0f;
    auto sidechainHz = 0.0;
    juce::String dragId;
    auto dragPixels = 0.0f;
    juce::Component* held = nullptr;

    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg { argv[i] };

        if (arg == "settings")
        {
            editor->showSettings (true);
            continue;
        }

        const auto key = arg.upToFirstOccurrenceOf ("=", false, false);
        const auto value = arg.fromFirstOccurrenceOf ("=", false, false);

        if (key == arg)
        {
            std::printf ("not a key=value argument: %s\n", argv[i]);
            return 1;
        }

        if (key == "scale")      { scale = value.getFloatValue(); continue; }
        if (key == "hover")      { hoverId = value; continue; }
        if (key == "audio")      { sidechainHz = value.getDoubleValue(); continue; }
        // drag=<id> just raises the dragging state; drag=<id>:<dy> presses and
        // pulls it that many pixels, negative being upwards.
        if (key == "drag")
        {
            dragId = value.upToFirstOccurrenceOf (":", false, false);
            dragPixels = value.fromFirstOccurrenceOf (":", false, false).getFloatValue();
            continue;
        }

        if (key == "crop")
        {
            juce::StringArray parts;
            parts.addTokens (value, ",", "");

            if (parts.size() != 4)
            {
                std::printf ("crop wants x,y,w,h: %s\n", argv[i]);
                return 1;
            }

            crop = { parts[0].getIntValue(), parts[1].getIntValue(),
                     parts[2].getIntValue(), parts[3].getIntValue() };
            continue;
        }

        auto* param = processor.apvts.getParameter (key);

        if (param == nullptr)
        {
            std::printf ("no such parameter: %s\n", key.toRawUTF8());
            return 1;
        }

        param->setValueNotifyingHost (param->convertTo0to1 (value.getFloatValue()));
    }

    // Without this the scope has nothing to draw and the most intricate part of
    // the editor cannot be checked at all. A sub on the sidechain against a
    // higher carrier is the case the plugin exists for.
    if (sidechainHz > 0.0)
    {
        const auto channels = processor.getTotalNumInputChannels();
        juce::AudioBuffer<float> buffer { juce::jmax (2, channels), 512 };
        juce::MidiBuffer midi;

        auto carrierPhase = 0.0, sidechainPhase = 0.0;
        const auto step = juce::MathConstants<double>::twoPi / 48000.0;

        for (int block = 0; block < 60; ++block)
        {
            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                const auto carrier = 0.8f * (float) std::sin (carrierPhase);
                const auto sidechain = 0.95f * (float) std::sin (sidechainPhase);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample (ch, n, ch < 2 ? carrier : sidechain);

                carrierPhase += step * sidechainHz * 11.0;
                sidechainPhase += step * sidechainHz;
            }

            processor.processBlock (buffer, midi);
        }
    }

    // Parameter changes reach the editor through async listener callbacks, and
    // nothing pumps the message queue here.
    editor->refreshFromParameters();

    // Same idea as hover: a drag is normally announced by JUCE's mouse handling,
    // and these are the public callbacks it would fire.
    if (dragId.isNotEmpty())
    {
        auto* target = findById (*editor, dragId);

        if (target == nullptr)
        {
            std::printf ("no component with id: %s\n", dragId.toRawUTF8());
            return 1;
        }

        if (dragPixels != 0.0f)
        {
            dragBy (*target, dragPixels);
            held = target;
        }
        else if (auto* slider = dynamic_cast<juce::Slider*> (target); slider != nullptr && slider->onDragStart)
        {
            slider->onDragStart();
        }
        else if (auto* pill = dynamic_cast<Pill*> (target); pill != nullptr && pill->onDragActive)
        {
            pill->onDragActive (true);
        }
        else if (auto* display = dynamic_cast<ScopeComponent*> (target);
                 display != nullptr && display->onDragActive)
        {
            display->onDragActive (true);
        }
        else
        {
            std::printf ("nothing draggable with id: %s\n", dragId.toRawUTF8());
            return 1;
        }

        editor->refreshFromParameters();
    }

    if (hoverId.isNotEmpty())
    {
        auto* target = findById (*editor, hoverId);

        if (target == nullptr)
        {
            std::printf ("no component with id: %s\n", hoverId.toRawUTF8());
            return 1;
        }

        setHovered (*target, true);
    }

    const auto area = crop.isEmpty() ? editor->getLocalBounds() : crop;
    const auto image = editor->createComponentSnapshot (area, true, scale);

    if (held != nullptr)
        releaseDrag (*held, dragPixels);

    const juce::File out { juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) };
    out.deleteFile();

    juce::FileOutputStream stream { out };
    juce::PNGImageFormat png;

    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::printf ("could not write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(),
                 image.getWidth(), image.getHeight());
    return 0;
}
