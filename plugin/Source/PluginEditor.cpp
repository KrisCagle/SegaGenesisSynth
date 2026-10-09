#include "PluginEditor.h"

namespace
{
    constexpr int kTopBarHeight = 40;
    constexpr int kKeyboardHeight = 90;
}

GenisysEditor::GenisysEditor (GenisysProcessor& p)
    : AudioProcessorEditor (p),
      genisys (p),
      parameters (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    presetLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);

    for (int i = 0; i < genisys.getNumPrograms(); ++i)
        presetBox.addItem (genisys.getProgramName (i), i + 1); // ComboBox IDs must be non-zero
    presetBox.setSelectedId (genisys.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { genisys.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    drumsButton.onClick = [this] { setKeyboardToDrums (drumsButton.getToggleState()); };
    addAndMakeVisible (drumsButton);

    hint.setText ("Click the keys, or play A W S E D F T G Y H U J K on your computer keyboard",
                  juce::dontSendNotification);
    hint.setFont (juce::FontOptions (13.0f));
    hint.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (hint);

    addAndMakeVisible (parameters);

    keyboard.setOctaveForMiddleC (4);  // label MIDI 60 as C4, the most common convention
    keyboard.setKeyPressBaseOctave (5); // computer keys start at middle C
    keyboard.setLowestVisibleKey (36);
    keyboard.setKeyWidth (24.0f);
    addAndMakeVisible (keyboard);

    setResizable (true, true);
    setResizeLimits (640, 420, 2000, 1600);
    setSize (820, 640);
}

void GenisysEditor::setKeyboardToDrums (bool drums)
{
    keyboard.setMidiChannel (drums ? 10 : 1);
    keyboard.setMidiChannelsToDisplay (drums ? (1 << 9) : 0xFFFF);

    // The kit only answers channel 10 while Drums On is enabled.
    if (drums)
        if (auto* drumsOn = genisys.state.getParameter ("drums_on"))
            drumsOn->setValueNotifyingHost (1.0f);
}

void GenisysEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void GenisysEditor::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (kTopBarHeight).reduced (8, 6);
    presetLabel.setBounds (top.removeFromLeft (60));
    presetBox.setBounds (top.removeFromLeft (180));
    top.removeFromLeft (16);
    drumsButton.setBounds (top.removeFromLeft (280));

    auto bottom = area.removeFromBottom (kKeyboardHeight + 22);
    hint.setBounds (bottom.removeFromTop (22).reduced (8, 0));
    keyboard.setBounds (bottom);

    parameters.setBounds (area);
}
