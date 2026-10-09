#include "PluginEditor.h"

#include "genisys_presets.h"

namespace
{
    constexpr int kTopBarHeight = 40;
    constexpr int kKeyboardHeight = 90;
    constexpr int kLowestKey = 36;  // C2: the drum kit starts here
    constexpr int kHighestKey = 84; // C6
    constexpr float kKeyboardVelocity = 100.0f / 127.0f;

    const char* kSynthHint = "Click the keys, or play A W S E D F T G Y H U J K on your computer keyboard. "
                             "Z / X: octave down / up";
    const char* kDrumHint = "A Kick  W Rim  S Snare  E Clap  D Snare 2  T Closed hat  Y Pedal hat  U Open hat  "
                            "O Crash  P Ride  F G H J K L Toms";
}

GenisysEditor::GenisysEditor (GenisysProcessor& p)
    : AudioProcessorEditor (p),
      genisys (p),
      parameters (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    presetLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);

    juce::String lastCategory;
    for (int i = 0; i < genisys.getNumPrograms(); ++i)
    {
        const juce::String category (genisys_preset_category (i));
        if (category != lastCategory)
        {
            presetBox.addSectionHeading (category);
            lastCategory = category;
        }
        presetBox.addItem (genisys.getProgramName (i), i + 1); // ComboBox IDs must be non-zero
    }
    presetBox.setSelectedId (genisys.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { genisys.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    drumsButton.onClick = [this] { setKeyboardToDrums (drumsButton.getToggleState()); };
    addAndMakeVisible (drumsButton);

    loadButton.setTooltip ("Load a Genesis FM patch: .tfi (TFM Music Maker), .vgi (VGM Music Maker) or .dmp (DefleMask)");
    saveButton.setTooltip ("Save the current FM sound as a .tfi file other Genesis tools can open");
    loadButton.onClick = [this] { loadPatch(); };
    saveButton.onClick = [this] { savePatch(); };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    status.setFont (juce::FontOptions (13.0f));
    status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (status);

    octaveDown.onClick = [this] { shiftOctave (-1); };
    octaveUp.onClick = [this] { shiftOctave (1); };
    octaveLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (octaveDown);
    addAndMakeVisible (octaveLabel);
    addAndMakeVisible (octaveUp);

    hint.setFont (juce::FontOptions (13.0f));
    hint.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (hint);

    addAndMakeVisible (parameters);

    // On-screen wheels, for playing without a MIDI controller. The bend
    // wheel springs back to centre on release, like a hardware one.
    bendWheel.setRange (-1.0, 1.0);
    bendWheel.setValue (0.0, juce::dontSendNotification);
    bendWheel.setDoubleClickReturnValue (true, 0.0);
    bendWheel.onValueChange = [this]
    {
        const int value = juce::jlimit (0, 16383, 8192 + (int) std::lround (bendWheel.getValue() * 8191.0));
        sendToProcessor (juce::MidiMessage::pitchWheel (1, value));
    };
    bendWheel.onDragEnd = [this] { bendWheel.setValue (0.0); };
    modWheel.setRange (0.0, 127.0, 1.0);
    modWheel.onValueChange = [this]
    {
        sendToProcessor (juce::MidiMessage::controllerEvent (1, 1, (int) modWheel.getValue()));
    };
    for (auto* label : { &bendLabel, &modLabel })
    {
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (label);
    }
    addAndMakeVisible (bendWheel);
    addAndMakeVisible (modWheel);

    // A fixed velocity: by default the keyboard derives velocity from where
    // on the key you click, which made clicks near the top almost silent.
    keyboard.setVelocity (kKeyboardVelocity, false);
    keyboard.setOctaveForMiddleC (4); // label MIDI 60 as C4, the most common convention
    keyboard.setAvailableRange (kLowestKey, kHighestKey);
    keyboard.setScrollButtonsVisible (false);
    addAndMakeVisible (keyboard);

    setKeyboardToDrums (false);
    timerCallback();
    startTimerHz (10); // keep the octave readout in sync with automation/presets

    setResizable (true, true);
    setResizeLimits (760, 420, 2400, 1600);
    setSize (980, 680);
}

void GenisysEditor::setKeyboardToDrums (bool drums)
{
    keyboard.setMidiChannel (drums ? 10 : 1);
    keyboard.setMidiChannelsToDisplay (drums ? (1 << 9) : 0xFFFF);
    // Computer keys start at C2 for the drum kit, middle C for playing.
    keyboard.setKeyPressBaseOctave (drums ? 3 : 5);
    hint.setText (drums ? kDrumHint : kSynthHint, juce::dontSendNotification);
    octaveDown.setEnabled (! drums); // drums aren't transposed
    octaveUp.setEnabled (! drums);

    // The kit only answers channel 10 while Drums On is enabled.
    if (drums)
        if (auto* drumsOn = genisys.state.getParameter ("drums_on"))
            drumsOn->setValueNotifyingHost (1.0f);
}

void GenisysEditor::loadPatch()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a Genesis FM patch", juce::File(), "*.tfi;*.vgi;*.dmp");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              const auto error = genisys.importPatchFile (file);
                              status.setText (error.isEmpty() ? "Loaded " + file.getFileName() : error,
                                              juce::dontSendNotification);
                          });
}

void GenisysEditor::savePatch()
{
    chooser = std::make_unique<juce::FileChooser> ("Save as a TFI patch", juce::File(), "*.tfi");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension (".tfi");
                              const auto error = genisys.exportPatchFile (file);
                              status.setText (error.isEmpty() ? "Saved " + file.getFileName() : error,
                                              juce::dontSendNotification);
                          });
}

void GenisysEditor::sendToProcessor (const juce::MidiMessage& message)
{
    auto timestamped = message;
    timestamped.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    genisys.editorMidi.addMessageToQueue (timestamped);
}

void GenisysEditor::shiftOctave (int delta)
{
    if (auto* octave = genisys.state.getParameter ("octave"))
    {
        const float current = octave->convertFrom0to1 (octave->getValue());
        octave->beginChangeGesture();
        octave->setValueNotifyingHost (octave->convertTo0to1 (current + (float) delta));
        octave->endChangeGesture();
    }
    timerCallback();
}

bool GenisysEditor::keyPressed (const juce::KeyPress& key)
{
    // The on-screen keyboard handles its note keys and passes others up here.
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    if (c == 'z') { shiftOctave (-1); return true; }
    if (c == 'x') { shiftOctave (1); return true; }
    return false;
}

void GenisysEditor::timerCallback()
{
    const int octave = (int) std::lround (genisys.state.getRawParameterValue ("octave")->load());
    octaveLabel.setText ("Octave " + juce::String (octave > 0 ? "+" : "") + juce::String (octave),
                         juce::dontSendNotification);
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
    top.removeFromLeft (8);
    loadButton.setBounds (top.removeFromLeft (100));
    top.removeFromLeft (4);
    saveButton.setBounds (top.removeFromLeft (100));
    top.removeFromLeft (12);
    drumsButton.setBounds (top.removeFromLeft (260));
    status.setBounds (top);

    auto bottom = area.removeFromBottom (kKeyboardHeight + 28);
    auto controls = bottom.removeFromTop (28).reduced (8, 3);
    octaveDown.setBounds (controls.removeFromLeft (56));
    octaveLabel.setBounds (controls.removeFromLeft (90));
    octaveUp.setBounds (controls.removeFromLeft (56));
    controls.removeFromLeft (12);
    hint.setBounds (controls);

    auto wheels = bottom.removeFromLeft (84).reduced (4, 2);
    auto bendArea = wheels.removeFromLeft (wheels.getWidth() / 2);
    bendLabel.setBounds (bendArea.removeFromBottom (16));
    bendWheel.setBounds (bendArea);
    modLabel.setBounds (wheels.removeFromBottom (16));
    modWheel.setBounds (wheels);

    // Fit every white key from C2 to C6 (29 of them) across the window.
    keyboard.setKeyWidth ((float) bottom.getWidth() / 29.0f);
    keyboard.setBounds (bottom);

    parameters.setBounds (area);
}
