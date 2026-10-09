#pragma once

#include <array>
#include <memory>

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace genisys::ui
{
    // "GENISYS" in chrome pixel letters with the red 16-bit stripe.
    class Logo : public juce::Component
    {
    public:
        void paint (juce::Graphics&) override;
    };

    // The preset display, styled as a cartridge label, with arrows.
    class PresetCartridge : public juce::Component
    {
    public:
        explicit PresetCartridge (GenisysProcessor&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void refresh();

    private:
        juce::Rectangle<float> leftArrow() const, rightArrow() const, label() const;
        void step (int delta);
        GenisysProcessor& processor;
        juce::String category, name;
        int shownProgram = -1;
        juce::uint32 lastStateHash = 0;
    };

    // Round, controller-style buttons (A / B / C).
    class PadButton : public juce::Component
    {
    public:
        PadButton (juce::String letter, juce::String caption);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void setCaption (const juce::String& c) { caption = c; repaint(); }
        std::function<void()> onClick;

    private:
        juce::String letter, caption;
        bool pressed = false;
    };

    // The 8 FM algorithms as clickable diagrams; carriers drawn in yellow.
    class AlgorithmPicker : public juce::Component
    {
    public:
        explicit AlgorithmPicker (juce::RangedAudioParameter& algorithm);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        int getAlgorithm() const { return selected; }
        static juce::String describe (int algorithm);

    private:
        juce::Rectangle<float> cell (int index) const;
        void drawDiagram (juce::Graphics&, juce::Rectangle<float>, int algorithm, bool on) const;
        juce::RangedAudioParameter& param;
        juce::ParameterAttachment attachment;
        int selected = 0;
    };

    // The shape of an operator's envelope, simulated with the real chip
    // envelope generator so what you see is what you hear.
    class EnvelopeGraph : public juce::Component
    {
    public:
        void setParams (int ar, int d1r, int d2r, int sl, int rr, int ks);
        void paint (juce::Graphics&) override;

    private:
        std::array<int, 6> params { -1, -1, -1, -1, -1, -1 };
        std::array<float, 100> curve {};
    };

    // One FM operator: tone, level, envelope and extras.
    class OperatorCard : public MenuWindow
    {
    public:
        OperatorCard (juce::AudioProcessorValueTreeState&, int op);
        void resized() override;
        void paint (juce::Graphics&) override;
        void setCarrier (bool isCarrier);
        void refreshEnvelope();

    private:
        juce::AudioProcessorValueTreeState& state;
        int op;
        bool carrier = false;
        std::vector<std::unique_ptr<juce::Component>> owned;
        RetroKnob *ratio, *level, *detune, *attack, *decay, *sustain, *release, *fade, *keyScale, *shape;
        LedToggle *tremolo, *ssg;
        EnvelopeGraph envelope;
    };

    // Drum pads that play the kit (MIDI channel 10).
    class DrumPads : public juce::Component
    {
    public:
        explicit DrumPads (GenisysProcessor&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;

    private:
        int padAt (juce::Point<float>) const;
        juce::Rectangle<float> padBounds (int index) const;
        GenisysProcessor& processor;
        int down = -1;
    };

    // CRT-style output scope with the six chip-channel lights.
    class ScopeScreen : public juce::Component, private juce::Timer
    {
    public:
        explicit ScopeScreen (GenisysProcessor&);
        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;
        GenisysProcessor& processor;
        std::array<float, 512> history {};
        int writePos = 0;
        uint8_t voices = 0;
    };

    // Pitch bend (springs back) and mod wheel, sending real MIDI.
    class Wheels : public juce::Component
    {
    public:
        explicit Wheels (GenisysProcessor&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> slot (int which) const;
        void send (int which, float value);
        GenisysProcessor& processor;
        float bend = 0.5f, mod = 0.0f;
        int dragging = -1;
    };

    // Adds a MIDI message from the UI to the processor's queue.
    void sendMidi (GenisysProcessor&, const juce::MidiMessage&);
}
