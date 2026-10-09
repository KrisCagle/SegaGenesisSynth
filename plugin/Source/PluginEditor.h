#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

// Interim editor: preset menu, every parameter (JUCE's generic list), and an
// on-screen keyboard so the Standalone app can be played without a MIDI
// controller. Phase 5 of the roadmap replaces this with the themed UI.
class GenisysEditor final : public juce::AudioProcessorEditor
{
public:
    explicit GenisysEditor (GenisysProcessor&);
    ~GenisysEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void setKeyboardToDrums (bool drums);

    GenisysProcessor& genisys;

    juce::Label presetLabel { {}, "Preset" };
    juce::ComboBox presetBox;
    juce::ToggleButton drumsButton { "Keyboard plays drums (MIDI ch 10)" };
    juce::Label hint;

    juce::GenericAudioProcessorEditor parameters;
    juce::MidiKeyboardComponent keyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenisysEditor)
};
