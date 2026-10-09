#pragma once

#include <functional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PixelFont.h"
#include "Theme.h"

namespace genisys::ui
{
    // Every widget can carry a plain-English explanation, shown in the help
    // box when the mouse is over it.
    void setHelp (juce::Component&, const juce::String& title, const juce::String& text);
    juce::String getHelp (const juce::Component&); // searches up the parent chain

    // A blue, beveled window like a 16-bit game menu, with a yellow title.
    class MenuWindow : public juce::Component
    {
    public:
        explicit MenuWindow (juce::String title = {});
        void setTitle (const juce::String& t) { title = t; repaint(); }
        void paint (juce::Graphics&) override;

        // Area below the title, inside the bevel.
        juce::Rectangle<int> getContentArea() const;

    private:
        juce::String title;
    };

    // Rotary knob with a ring of lit segments, bound to a host parameter.
    // Drag up/down (Shift = fine), mouse wheel, double-click resets.
    class RetroKnob : public juce::Component
    {
    public:
        using Formatter = std::function<juce::String (float value)>;

        RetroKnob (juce::RangedAudioParameter&, juce::String label, bool small = false);

        // Inverted knobs turn up as the register value goes down: used where
        // the chip counts "backwards" (TL: 0 = loudest; rates: 31 = fastest),
        // so turning a knob up always means "more".
        RetroKnob& inverted (bool shouldInvert = true);
        RetroKnob& withFormatter (Formatter);
        RetroKnob& withHelp (const juce::String& title, const juce::String& text);
        void setLabel (const juce::String&);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        static constexpr int normalWidth = 76, normalHeight = 96;
        static constexpr int smallWidth = 52, smallHeight = 80;

    private:
        float position() const; // 0..1 around the ring
        void setFromPosition (float pos, bool asGesture);

        juce::RangedAudioParameter& param;
        juce::ParameterAttachment attachment;
        juce::String label;
        bool small, invert = false;
        Formatter formatter;
        float value = 0.0f;          // denormalised parameter value
        float dragStartPos = 0.0f;
        bool hovering = false;
    };

    // A row of options, one lit (e.g. POLY / MONO / LEGATO).
    class SegmentedChoice : public juce::Component
    {
    public:
        SegmentedChoice (juce::RangedAudioParameter&, juce::StringArray labels);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        juce::RangedAudioParameter& param;
        juce::ParameterAttachment attachment;
        juce::StringArray labels;
        int selected = 0;
    };

    // On/off switch with a red LED, bound to an on/off parameter, or free
    // standing (no parameter) with an onChange callback.
    class LedToggle : public juce::Component
    {
    public:
        LedToggle (juce::RangedAudioParameter&, juce::String label);
        explicit LedToggle (juce::String label);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        bool isOn() const { return on; }
        void setOn (bool shouldBeOn);
        std::function<void (bool)> onChange;

    private:
        std::unique_ptr<juce::ParameterAttachment> attachment;
        juce::RangedAudioParameter* param = nullptr;
        juce::String label;
        bool on = false;
    };

    // Small beveled pixel-text button.
    class PixelButton : public juce::Component
    {
    public:
        explicit PixelButton (juce::String text);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void setText (const juce::String& t) { text = t; repaint(); }
        std::function<void()> onClick;

    private:
        juce::String text;
        bool pressed = false;
    };

    // The game-style text box: types out help text, with a blinking arrow.
    class HelpBox : public juce::Component, private juce::Timer
    {
    public:
        HelpBox();
        void show (const juce::String& help); // "TITLE|text"
        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;
        juce::String title, body;
        int shown = 0;
        int blink = 0;
    };

    // Game-menu tabs with a blinking cursor on the active one.
    class TabBar : public juce::Component, private juce::Timer
    {
    public:
        explicit TabBar (juce::StringArray names);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        int getSelected() const { return selected; }
        void setSelected (int index) { selected = index; repaint(); }
        std::function<void (int)> onChange;
        juce::StringArray helpTexts; // per tab

    private:
        void timerCallback() override;
        juce::Rectangle<float> tabBounds (int index) const;
        juce::StringArray names;
        int selected = 0;
        bool cursorVisible = true;
    };
}
