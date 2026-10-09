#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

// Interim editor: preset menu, every parameter (JUCE's generic list), and an
// on-screen keyboard so the Standalone app can be played without a MIDI
// controller. Phase 5 of the roadmap replaces this with the themed UI.
class GenisysEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit GenisysEditor (GenisysProcessor&);
    ~GenisysEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void setKeyboardToDrums (bool drums);
    void shiftOctave (int delta);
    void loadPatch();
    void savePatch();

    GenisysProcessor& genisys;

    juce::Label presetLabel { {}, "Preset" };
    juce::ComboBox presetBox;
    juce::ToggleButton drumsButton { "Keyboard plays drums (MIDI ch 10)" };
    juce::TextButton loadButton { "Load Patch..." }, saveButton { "Save Patch..." };
    juce::Label status;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::TextButton octaveDown { "Oct -" }, octaveUp { "Oct +" };
    juce::Label octaveLabel;
    juce::Label hint;

    void sendToProcessor (const juce::MidiMessage& message);

    juce::Slider bendWheel { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider modWheel { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Label bendLabel { {}, "Bend" }, modLabel { {}, "Mod" };

    juce::GenericAudioProcessorEditor parameters;
    juce::MidiKeyboardComponent keyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenisysEditor)
};
