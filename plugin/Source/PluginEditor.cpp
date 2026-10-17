#include "PluginEditor.h"

#include "genisys_presets.h"

using namespace genisys::ui;

namespace
{
    constexpr int kKeyboardLowest = 36;  // C2: the drum kit starts here
    constexpr int kKeyboardHighest = 84; // C6

    // Static pixel text: subheadings, legends, notes.
    class PixelText : public juce::Component
    {
    public:
        PixelText (juce::String t, float size, juce::Colour c, bool wrapText = false)
            : text (std::move (t)), px (size), colour (c), wrap (wrapText)
        {
            setInterceptsMouseClicks (false, false);
        }
        void paint (juce::Graphics& g) override
        {
            if (wrap)
                PixelFont::drawWrapped (g, text.toUpperCase(), getLocalBounds().toFloat(), px, colour);
            else
                PixelFont::drawIn (g, text.toUpperCase(), getLocalBounds().toFloat(), px, colour, juce::Justification::centredLeft);
        }

    private:
        juce::String text;
        float px;
        juce::Colour colour;
        bool wrap;
    };

    juce::String hz (float v) { return juce::String (v, 1) + "HZ"; }
}

// ---------------------------------------------------------------------------
void GenisysEditor::Canvas::paint (juce::Graphics& g)
{
    // Black console chassis with fine vertical ribbing.
    g.setGradientFill (juce::ColourGradient (theme::chassisLight, 0.0f, 0.0f, theme::chassis, 0.0f, (float) getHeight(), false));
    g.fillAll();
    g.setColour (juce::Colours::white.withAlpha (0.015f));
    for (int x = 0; x < getWidth(); x += 6)
        g.fillRect (x, 0, 2, getHeight());
}

void GenisysEditor::Canvas::paintOverChildren (juce::Graphics& g)
{
    if (! scanlines)
        return;
    // CRT scanlines over everything.
    g.setColour (juce::Colours::black.withAlpha (0.16f));
    for (int y = 0; y < getHeight(); y += 3)
        g.fillRect (0, y, getWidth(), 1);
}

void GenisysEditor::OctaveReadout::paint (juce::Graphics& g)
{
    PixelFont::drawIn (g, "OCT " + juce::String (octave > 0 ? "+" : "") + juce::String (octave), getLocalBounds().toFloat(), 1.15f,
                       theme::yellow, juce::Justification::centred);
}

// ---------------------------------------------------------------------------
GenisysEditor::GenisysEditor (GenisysProcessor& p)
    : AudioProcessorEditor (p),
      genisys (p),
      cartridge (p),
      learnOverlay (p.midiMap),
      scope (p),
      wheels (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    // Pop-up menus in the panel's colours.
    lookAndFeel.setColour (juce::PopupMenu::backgroundColourId, theme::menuBottom);
    lookAndFeel.setColour (juce::PopupMenu::textColourId, theme::text);
    lookAndFeel.setColour (juce::PopupMenu::headerTextColourId, theme::yellow);
    lookAndFeel.setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::yellow);
    lookAndFeel.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (0xff111111));
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (canvas);
    canvas.setSize (theme::width, theme::height);

    // Header.
    canvas.addAndMakeVisible (logo);
    logo.setBounds (18, 14, 270, 54);
    canvas.addAndMakeVisible (cartridge);
    cartridge.setBounds (300, 14, 520, 54);
    for (auto* b : { &loadButton, &saveButton, &compareButton })
        canvas.addAndMakeVisible (*b);
    loadButton.setBounds (836, 12, 48, 58);
    saveButton.setBounds (886, 12, 48, 58);
    compareButton.setBounds (936, 12, 52, 58);
    setHelp (loadButton, "A - LOAD",
             "Load a Genisys preset file a friend sent you, or import a Genesis FM patch from TFM Music Maker (.TFI), VGM Music "
             "Maker (.VGI) or DefleMask (.DMP).");
    setHelp (saveButton, "B - SAVE", "Save this sound as one of your presets (it appears under MY PRESETS), or export the FM part as a .TFI patch.");
    setHelp (compareButton, "C - COMPARE", "Flip between two versions of your sound (A and B) to compare edits.");
    loadButton.onClick = [this] { showLoadMenu(); };
    saveButton.onClick = [this] { showSaveMenu(); };
    cartridge.onSaveRequested = [this] { saveUserPreset(); };
    lookAndFeel.setColour (juce::AlertWindow::backgroundColourId, theme::menuBottom);
    lookAndFeel.setColour (juce::AlertWindow::textColourId, theme::text);
    lookAndFeel.setColour (juce::AlertWindow::outlineColourId, theme::bevelLight);
    lookAndFeel.setColour (juce::TextEditor::backgroundColourId, theme::screen);
    lookAndFeel.setColour (juce::TextEditor::textColourId, theme::cyan);
    lookAndFeel.setColour (juce::TextEditor::outlineColourId, theme::screenEdge);
    lookAndFeel.setColour (juce::TextButton::buttonColourId, theme::menuTop);
    compareButton.onClick = [this] { toggleCompare(); };

    // Tabs and pages.
    canvas.addAndMakeVisible (tabs);
    tabs.setBounds (16, 80, 968, 30);
    tabs.helpTexts = { "The big picture: algorithm, quick sound shaping and how notes play.",
                       "Deep editing of the 4 FM operators.",
                       "The square-wave chip: layers, arpeggios and noise.",
                       "The 8-bit drum kit on MIDI channel 10.",
                       "Chorus, echo and reverb.",
                       "Vibrato, tremolo, pitch bend and display options.",
                       "Link the knobs and faders on your MIDI keyboard to Genisys." };
    setHelp (tabs, "MENU", "Click a tab to switch pages.");
    tabs.onChange = [this] (int i) { showPage (i); };
    for (auto& page : pages)
    {
        canvas.addChildComponent (page);
        page.setBounds (16, 110, 968, 372);
    }
    buildVoicePage (pages[0]);
    buildOperatorsPage (pages[1]);
    buildPsgPage (pages[2]);
    buildDrumsPage (pages[3]);
    buildFxPage (pages[4]);
    buildModPage (pages[5]);
    buildMidiPage (pages[6]);

    canvas.addChildComponent (learnOverlay);
    learnOverlay.setBounds (pages[0].getBounds());
    learnOverlay.onPick = [this] (const juce::String& id)
    {
        genisys.midiMap.startLearning (id);
        help.show ("MIDI LEARN|Now move a knob or fader on your MIDI keyboard to link it to " + genisys.midiMap.parameterName (id) + ".");
    };
    learnOverlay.onForget = [this] (const juce::String& id)
    {
        genisys.midiMap.forgetParam (id);
        help.show ("MIDI LEARN|Unlinked " + genisys.midiMap.parameterName (id) + ".");
    };
    canvas.addAndMakeVisible (learnButton);
    learnButton.setBounds (884, 82, 100, 26);
    learnButton.onClick = [this] { setLearnMode (! learnMode); };
    setHelp (learnButton, "MIDI LEARN",
             "Link your MIDI keyboard's knobs to Genisys: press LEARN, click a control, then turn a knob. Right-click any control "
             "for the same thing. Esc or LEARN again to finish.");
    showPage (0);
    shownMapVersion = genisys.midiMap.getVersion(); // opening the editor changes nothing

    // Bottom: help, scope, wheels, octave, keyboard.
    canvas.addAndMakeVisible (help);
    help.setBounds (16, 492, 732, 58);
    canvas.addAndMakeVisible (scope);
    scope.setBounds (756, 492, 228, 186);
    canvas.addAndMakeVisible (wheels);
    wheels.setBounds (16, 558, 58, 120);
    for (auto* b : { &octaveUp, &octaveDown })
        canvas.addAndMakeVisible (*b);
    octaveUp.setBounds (80, 566, 64, 24);
    octaveDown.setBounds (80, 620, 64, 24);
    canvas.addAndMakeVisible (octaveReadout);
    octaveReadout.setBounds (80, 594, 64, 22);
    octaveUp.onClick = [this] { shiftOctave (1); };
    octaveDown.onClick = [this] { shiftOctave (-1); };
    setHelp (octaveUp, "OCTAVE", "Shift what you play up or down an octave. Keys Z and X do the same.");
    setHelp (octaveDown, "OCTAVE", "Shift what you play up or down an octave. Keys Z and X do the same.");

    canvas.addAndMakeVisible (keyboard);
    keyboard.setBounds (150, 558, 598, 120);
    keyboard.setAvailableRange (kKeyboardLowest, kKeyboardHighest);
    keyboard.setScrollButtonsVisible (false);
    keyboard.setKeyWidth (598.0f / 29.0f);
    keyboard.setOctaveForMiddleC (4);
    keyboard.setVelocity (100.0f / 127.0f, false);
    keyboard.setColour (juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour (0xffe9e9e9));
    keyboard.setColour (juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour (0xff141418));
    keyboard.setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff222222));
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, theme::yellow);
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, theme::cyan.withAlpha (0.35f));
    keyboard.setColour (juce::MidiKeyboardComponent::shadowColourId, juce::Colours::black.withAlpha (0.5f));
    setHelp (keyboard, "KEYBOARD",
             "Click to play, or use A W S E D F T G Y H U J K on your computer keyboard. Z / X change octave. LEFT / RIGHT arrows "
             "change preset.");
    setKeyboardToDrums (false);

    // Hover help: listen to mouse movement anywhere on the canvas.
    canvas.addMouseListener (this, true);

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) theme::width / (double) theme::height);
    setResizeLimits (theme::width * 7 / 10, theme::height * 7 / 10, theme::width * 2, theme::height * 2);
    setSize (theme::width, theme::height);

    timerCallback();
    startTimerHz (15);
}

GenisysEditor::~GenisysEditor()
{
    genisys.midiMap.stopLearning();
    canvas.removeMouseListener (this);
    setLookAndFeel (nullptr);
}

juce::RangedAudioParameter& GenisysEditor::param (const char* id)
{
    auto* p = genisys.state.getParameter (id);
    jassert (p != nullptr);
    return *p;
}

void GenisysEditor::resized()
{
    const float scale = (float) getWidth() / (float) theme::width;
    canvas.setTransform (juce::AffineTransform::scale (scale));
}

void GenisysEditor::selectTab (int index)
{
    tabs.setSelected (index);
    showPage (index);
}

void GenisysEditor::showPage (int index)
{
    currentPage = index;
    for (size_t i = 0; i < pages.size(); ++i)
        pages[i].setVisible ((int) i == index);
    learnOverlay.setPage (&pages[(size_t) index]);
    // The MIDI page stays usable while learning (to check the links).
    learnOverlay.setVisible (learnMode && index != numTabs - 1);
}

// ---------------------------------------------------------------------------
void GenisysEditor::setLearnMode (bool on)
{
    learnMode = on;
    if (! on)
        genisys.midiMap.stopLearning();
    if (on && currentPage == numTabs - 1)
        selectTab (0); // nothing to learn on the MIDI page itself
    learnOverlay.setVisible (on && currentPage != numTabs - 1);
    learnOverlay.toFront (false);
    learnButton.setLit (on);
    learnButton.setText (on ? "LEARNING" : "LEARN");
    help.show (on ? "MIDI LEARN|Click any outlined control, then move a knob or fader on your MIDI keyboard. Right-click an "
                    "outline to unlink it. Switch tabs to reach every page. Press LEARN or Esc when you're done."
                  : "MIDI LEARN|Done. Your links are saved with your project.");
}

void GenisysEditor::showParameterMenu (juce::Component& source, juce::RangedAudioParameter& p)
{
    const auto id = p.getParameterID();
    const int cc = genisys.midiMap.ccFor (id);

    juce::PopupMenu menu;
    menu.addSectionHeader (p.getName (40));
    menu.addItem (1, cc >= 0 ? "MIDI Learn (now CC " + juce::String (cc) + ")" : juce::String ("MIDI Learn"));
    menu.addItem (2, "Forget MIDI link", cc >= 0);
    menu.addSeparator();
    menu.addItem (3, "Reset to default");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source),
                        [this, id] (int result)
                        {
                            auto& m = genisys.midiMap;
                            auto* param = m.getParameter (id);
                            if (result == 1)
                            {
                                m.startLearning (id);
                                help.show ("MIDI LEARN|Move a knob or fader on your MIDI keyboard to link it to " + m.parameterName (id)
                                           + ". Esc cancels.");
                            }
                            else if (result == 2)
                            {
                                m.forgetParam (id);
                                help.show ("MIDI LEARN|Unlinked " + m.parameterName (id) + ".");
                            }
                            else if (result == 3 && param != nullptr)
                            {
                                param->beginChangeGesture();
                                param->setValueNotifyingHost (param->getDefaultValue());
                                param->endChangeGesture();
                            }
                        });
}

void GenisysEditor::showActionMenu (juce::Component& source, const juce::String& actionId)
{
    auto& map = genisys.midiMap;
    const int cc = map.ccFor (actionId);
    juce::PopupMenu menu;
    menu.addSectionHeader (map.parameterName (actionId));
    menu.addItem (1, cc >= 0 ? "MIDI Learn (now CC " + juce::String (cc) + ")" : juce::String ("MIDI Learn"));
    menu.addItem (2, "Forget MIDI link", cc >= 0);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source),
                        [this, actionId] (int result)
                        {
                            auto& m = genisys.midiMap;
                            if (result == 1)
                            {
                                m.startLearning (actionId);
                                help.show ("MIDI LEARN|Press the pad or button on your MIDI keyboard that should go to the "
                                           + m.parameterName (actionId) + ". Esc cancels.");
                            }
                            else if (result == 2)
                            {
                                m.forgetParam (actionId);
                                help.show ("MIDI LEARN|Unlinked " + m.parameterName (actionId) + ".");
                            }
                        });
}

void GenisysEditor::refreshMidiMap()
{
    auto& map = genisys.midiMap;
    const auto learning = map.learningParamId();

    // A knob was just learned (the audio thread finished it).
    if (lastLearning.isNotEmpty() && learning.isEmpty())
    {
        const int cc = map.ccFor (lastLearning);
        if (cc >= 0)
            help.show ("LINKED!|CC " + juce::String (cc) + " now controls " + map.parameterName (lastLearning)
                       + (learnMode ? ". Click another control, or press LEARN to finish." : "."));
    }
    lastLearning = learning;

    const auto version = map.getVersion();
    if (version != shownMapVersion)
    {
        shownMapVersion = version;
        if (mappingList != nullptr)
            mappingList->refresh();
        if (pickupToggle != nullptr && pickupToggle->isOn() != map.pickupEnabled())
            pickupToggle->setOn (map.pickupEnabled());
        learnOverlay.repaint();
        // The map is saved with the project, so the project has changed.
        genisys.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
    }

    if (learnMode && ++blinkTicks % 4 == 0)
        learnOverlay.blink();
}

// ---------------------------------------------------------------------------
void GenisysEditor::buildVoicePage (Page& page)
{
    auto& algo = page.add<MenuWindow> ({ 0, 0, 300, 372 }, "ALGORITHM");
    juce::ignoreUnused (algo);
    page.add<AlgorithmPicker> ({ 12, 30, 272, 330 }, param ("algorithm"));

    page.add<MenuWindow> ({ 310, 0, 408, 372 }, "QUICK SOUND");
    const int qx = 310 + (408 - 3 * RetroKnob::normalWidth) / 2;
    page.addAt<RetroKnob> ({ qx, 28 }, param ("macro_bright"), "BRIGHT")
        .withHelp ("BRIGHTNESS", "Turns every modulator up or down at once. Right = brighter and buzzier, left = softer. 0 = as the preset was made.");
    page.addAt<RetroKnob> ({ qx + RetroKnob::normalWidth, 28 }, param ("macro_attack"), "ATTACK")
        .withHelp ("ATTACK", "Right = notes fade in more slowly. Left = snappier. 0 = as programmed.");
    page.addAt<RetroKnob> ({ qx + 2 * RetroKnob::normalWidth, 28 }, param ("macro_decay"), "DECAY")
        .withHelp ("DECAY", "Right = the sound takes longer to drop after the hit. Left = shorter, more percussive.");
    page.addAt<RetroKnob> ({ qx, 126 }, param ("macro_release"), "RELEASE")
        .withHelp ("RELEASE", "Right = notes ring on longer after you let go.");
    page.addAt<RetroKnob> ({ qx + RetroKnob::normalWidth, 126 }, param ("feedback"), "FEEDBACK")
        .withHelp ("FEEDBACK", "Operator 1 feeds back into itself: adds grit and a saw-like edge. 7 = noisy.");
    page.addAt<RetroKnob> ({ qx + 2 * RetroKnob::normalWidth, 126 }, param ("vibrato_amount"), "VIBRATO")
        .withHelp ("VIBRATO", "Constant pitch wobble. The mod wheel can add more. Speed and depth are on the MOD tab.");

    page.add<PixelText> ({ 322, 228, 200, 14 }, "CONSOLE", 1.5f, theme::yellow);
    page.add<PixelText> ({ 324, 250, 200, 12 }, "CHIP", 1.15f, theme::text);
    auto& chip = page.add<SegmentedChoice> ({ 322, 266, 180, 26 }, param ("chip_model"), juce::StringArray { "YM2612", "YM3438", "CLEAN" });
    setHelp (chip, "CHIP", "YM2612: the gritty original Model 1 sound. YM3438: the cleaner later chip. CLEAN: no DAC grit at all.");
    auto& filterOn = page.add<LedToggle> ({ 324, 304, 170, 16 }, param ("console_filter"), "CONSOLE FILTER");
    setHelp (filterOn, "CONSOLE FILTER", "The warm low-pass filter of the console's output. Off = brighter than any real console.");
    page.addAt<RetroKnob> ({ 528, 246 }, param ("filter_cutoff"), "FILTER", true)
        .withHelp ("FILTER CUTOFF", "How dark the console filter is. A Model 1 is about 3.7 kHz.");
    page.addAt<RetroKnob> ({ 600, 246 }, param ("master_gain"), "VOLUME", true)
        .withHelp ("VOLUME", "Master output level, after the effects.");

    page.add<MenuWindow> ({ 728, 0, 240, 372 }, "PLAY MODE");
    page.add<PixelText> ({ 742, 30, 200, 12 }, "VOICES", 1.15f, theme::text);
    auto& mode = page.add<SegmentedChoice> ({ 740, 46, 212, 26 }, param ("voice_mode"), juce::StringArray { "POLY", "MONO", "LEGATO" });
    setHelp (mode, "VOICES", "POLY plays chords. MONO plays one note at a time. LEGATO also slides between overlapping notes without restarting them.");
    page.addAt<RetroKnob> ({ 760, 84 }, param ("glide_time"), "GLIDE")
        .withHelp ("GLIDE", "Slides between notes in MONO or LEGATO. Off in POLY.");
    page.addAt<RetroKnob> ({ 840, 84 }, param ("unison"), "UNISON")
        .withHelp ("UNISON", "Stacks 2 or 3 detuned copies of each note for a huge sound. Fewer notes fit in a chord.");
    page.addAt<RetroKnob> ({ 760, 184 }, param ("unison_detune"), "DETUNE")
        .withHelp ("DETUNE", "How far apart the unison copies are tuned.");
    page.addAt<RetroKnob> ({ 840, 184 }, param ("velocity_sens"), "VELOCITY")
        .withHelp ("VELOCITY", "How much harder playing gets louder. 0% = every note the same, like the real console.");
    auto& stereo = page.add<LedToggle> ({ 744, 300, 200, 16 }, param ("unison_stereo"), "UNISON STEREO");
    setHelp (stereo, "UNISON STEREO", "Spreads unison copies hard left and right using the chip's own panning, like games did.");
}

void GenisysEditor::buildOperatorsPage (Page& page)
{
    for (int op = 0; op < 4; ++op)
        cards[(size_t) op] = &page.add<OperatorCard> ({ op * 244, 0, 236, 372 }, genisys.state, op);
}

void GenisysEditor::buildPsgPage (Page& page)
{
    page.add<MenuWindow> ({ 0, 0, 478, 372 }, "SQUARE LAYER");
    page.add<PixelText> ({ 14, 30, 200, 12 }, "MODE", 1.15f, theme::text);
    auto& mode = page.add<SegmentedChoice> ({ 12, 46, 300, 26 }, param ("psg_mode"), juce::StringArray { "OFF", "UNISON", "ARPEGGIO" });
    setHelp (mode, "PSG MODE", "UNISON doubles your notes with square waves. ARPEGGIO cycles one square through the held notes, like classic chiptunes.");
    page.addAt<RetroKnob> ({ 20, 92 }, param ("psg_level"), "LEVEL").withHelp ("LEVEL", "Volume of the square layer.");
    page.addAt<RetroKnob> ({ 100, 92 }, param ("psg_octave"), "OCTAVE").withHelp ("OCTAVE", "Moves the squares up or down against the FM.");
    page.addAt<RetroKnob> ({ 180, 92 }, param ("psg_arp_speed"), "ARP SPEED").withHelp ("ARP SPEED", "How fast the arpeggio steps between notes.");
    page.add<PixelText> ({ 14, 210, 450, 120 },
                         "The SN76489 PSG: the Genesis's second chip. Three square waves and a noise channel. Its volume steps once per video frame, which gives it that stepped chiptune fade.",
                         1.2f, theme::textDim, true);

    page.add<MenuWindow> ({ 490, 0, 478, 372 }, "ENVELOPE (60 STEPS/SEC)");
    page.addAt<RetroKnob> ({ 510, 30 }, param ("psg_attack"), "ATTACK")
        .withHelp ("ATTACK", "Time per volume step on the way up (16 steps). 0 = instant.");
    page.addAt<RetroKnob> ({ 590, 30 }, param ("psg_decay"), "DECAY").withHelp ("DECAY", "Time per volume step down to the sustain level.");
    page.addAt<RetroKnob> ({ 670, 30 }, param ("psg_sustain"), "SUSTAIN").withHelp ("SUSTAIN", "Volume while the key is held.");
    page.addAt<RetroKnob> ({ 750, 30 }, param ("psg_release"), "RELEASE").withHelp ("RELEASE", "Time per volume step after you let go.");
    page.add<PixelText> ({ 504, 136, 200, 14 }, "NOISE", 1.5f, theme::yellow);
    auto& noiseOn = page.add<LedToggle> ({ 506, 160, 120, 16 }, param ("noise_on"), "NOISE ON");
    setHelp (noiseOn, "NOISE ON", "A constant noise layer from the PSG. Usually off; drum hits use this channel too.");
    auto& noiseType = page.add<SegmentedChoice> ({ 640, 156, 200, 24 }, param ("noise_white"), juce::StringArray { "PERIODIC", "WHITE" });
    setHelp (noiseType, "NOISE TYPE", "WHITE is hiss. PERIODIC is a buzzy, pitched tone.");
    page.addAt<RetroKnob> ({ 510, 194 }, param ("noise_rate"), "RATE", true).withHelp ("NOISE RATE", "Noise pitch. TONE 3 follows the third square channel.");
    page.addAt<RetroKnob> ({ 570, 194 }, param ("noise_volume"), "VOLUME", true).withHelp ("NOISE VOLUME", "Volume of the noise layer.");
}

void GenisysEditor::buildDrumsPage (Page& page)
{
    page.add<MenuWindow> ({ 0, 0, 700, 372 }, "DRUM KIT");
    page.add<DrumPads> ({ 12, 32, 674, 320 }, genisys);

    page.add<MenuWindow> ({ 710, 0, 258, 372 }, "KIT");
    auto& on = page.add<LedToggle> ({ 724, 32, 220, 16 }, param ("drums_on"), "DRUMS ON");
    setHelp (on, "DRUMS ON", "Gives FM channel 6 to the drum sampler, like the games did. FM then has 5 voices.");
    page.addAt<RetroKnob> ({ 724, 56 }, param ("drum_level"), "LEVEL").withHelp ("DRUM LEVEL", "Volume of the whole kit.");
    page.add<PixelText> ({ 726, 158, 220, 12 }, "DRUM NOTES FROM", 1.15f, theme::text);
    auto& input = page.add<SegmentedChoice> ({ 724, 174, 228, 26 }, param ("drum_input"), juce::StringArray { "CH 10", "ALL MIDI" });
    setHelp (input, "DRUM NOTES FROM",
             "CH 10: only MIDI channel 10 plays drums (the General MIDI standard). ALL MIDI: every note plays drums - use a second "
             "Genisys on its own track as a drum machine. Best for Ableton, which always sends channel 1.");
    auto& keys = page.add<LedToggle> ({ 724, 214, 230, 16 }, "KEYS PLAY DRUMS");
    setHelp (keys, "KEYS PLAY DRUMS", "Sends the on-screen and computer keyboard to the drum kit instead of the synth.");
    keys.onChange = [this] (bool drums) { setKeyboardToDrums (drums); };
    page.add<PixelText> ({ 724, 244, 230, 116 },
                         "Yellow pads: 8-bit samples through the FM chip's DAC. Cyan pads: PSG noise. Kick C2, snare D2, closed hat F#2.",
                         1.1f, theme::text, true);
}

void GenisysEditor::buildMidiPage (Page& page)
{
    using Layout = genisys::MidiMap::Layout;
    page.add<MenuWindow> ({ 0, 0, 300, 372 }, "CONTROLLER");
    page.add<PixelText> ({ 14, 30, 270, 12 }, "FACTORY LAYOUTS", 1.15f, theme::text);
    struct Choice { Layout layout; const char* label; const char* help; };
    const Choice choices[] = {
        { Layout::generic, "GENERIC (STANDARD CCS)",
          "Uses the MIDI standard's sound-control numbers (brightness on CC 74, attack 73, release 72...). Works with many keyboards and DAW templates." },
        { Layout::arturiaMiniLab, "ARTURIA MINILAB",
          "MiniLab 3 factory settings. Knobs 1-8: bright, attack, decay, release, feedback, vibrato, echo, reverb. Faders: volume, PSG, drums, glide." },
        { Layout::akaiMpkMini, "AKAI MPK MINI",
          "MPK Mini factory program (knobs send CC 70-77). Knobs 1-8: bright, attack, decay, release, feedback, vibrato, echo, reverb." },
        { Layout::novationLaunchkey, "NOVATION LAUNCHKEY",
          "Launchkey pots in Custom mode (CC 21-28). Knobs 1-8: bright, attack, decay, release, feedback, vibrato, echo, reverb." },
    };
    int y = 48;
    for (const auto& c : choices)
    {
        auto& b = page.add<PixelButton> ({ 14, y, 272, 24 }, c.label);
        setHelp (b, c.label, juce::String (c.help) + " Replaces the current links.");
        const auto layout = c.layout;
        b.onClick = [this, layout]
        {
            genisys.midiMap.loadLayout (layout);
            help.show ("CONTROLLER|Loaded the " + genisys::MidiMap::layoutName (layout).toUpperCase()
                       + " layout. Press SET AS DEFAULT to use it in every new Genisys.");
        };
        y += 30;
    }

    auto& pickup = page.add<LedToggle> ({ 16, 178, 260, 16 }, "PICKUP (NO JUMPS)");
    pickupToggle = &pickup;
    pickup.setOn (genisys.midiMap.pickupEnabled());
    setHelp (pickup, "PICKUP",
             "On: a hardware knob only takes over once you turn it past the control's current setting, so sounds never jump. "
             "Off: the control follows the knob straight away.");
    pickup.onChange = [this] (bool on) { genisys.midiMap.setPickup (on); };

    auto& setDefault = page.add<PixelButton> ({ 14, 210, 272, 24 }, "SET AS DEFAULT");
    setHelp (setDefault, "SET AS DEFAULT", "Every new Genisys you open starts with these links. Projects keep their own.");
    setDefault.onClick = [this]
    {
        help.show (genisys.midiMap.saveAsDefault() ? "SET AS DEFAULT|Saved. New Genisys windows start with these links."
                                                   : "SET AS DEFAULT|Couldn't save the default map.");
    };
    auto& load = page.add<PixelButton> ({ 14, 240, 132, 24 }, "LOAD MAP");
    setHelp (load, "LOAD MAP", "Open a controller map file (.genisysmap), e.g. one a friend made for the same keyboard.");
    load.onClick = [this] { loadControllerMap(); };
    auto& save = page.add<PixelButton> ({ 154, 240, 132, 24 }, "SAVE MAP");
    setHelp (save, "SAVE MAP", "Save these links as a file to share or keep.");
    save.onClick = [this] { saveControllerMap(); };
    auto& clear = page.add<PixelButton> ({ 14, 270, 272, 24 }, "CLEAR ALL");
    setHelp (clear, "CLEAR ALL", "Remove every link.");
    clear.onClick = [this] { genisys.midiMap.clear(); help.show ("CLEAR ALL|Every link removed."); };
    page.add<PixelText> ({ 14, 302, 272, 12 }, "PRESET BUTTONS", 1.15f, theme::text);
    struct PresetButton { juce::String id; const char* label; int x; };
    const PresetButton presetButtons[] = { { genisys::MidiMap::previousPresetId, "LEARN PREV", 14 },
                                           { genisys::MidiMap::nextPresetId, "LEARN NEXT", 154 } };
    for (const auto& pb : presetButtons)
    {
        auto& b = page.add<PixelButton> ({ pb.x, 318, 132, 24 }, pb.label);
        setHelp (b, "PRESET BUTTONS",
                 "Step through presets from your MIDI keyboard: click, then press a pad or button on it. The LEFT and RIGHT arrow "
                 "keys on your computer do the same.");
        const auto target = pb.id;
        b.onClick = [this, target]
        {
            genisys.midiMap.startLearning (target);
            help.show ("MIDI LEARN|Press the pad or button on your MIDI keyboard that should go to the "
                       + genisys.midiMap.parameterName (target) + ". Esc cancels.");
        };
    }

    page.add<MenuWindow> ({ 310, 0, 658, 372 }, "KNOB LINKS");
    mappingList = &page.add<MappingList> ({ 322, 32, 634, 326 }, genisys.midiMap);
    setHelp (*mappingList, "KNOB LINKS",
             "Drag MIN or MAX up and down to limit how far a knob moves its control (MIN above MAX turns it around). "
             "Double-click to reset. X removes a link.");
}

void GenisysEditor::loadControllerMap()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a controller map", juce::File(), "*.genisysmap");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              auto xml = juce::XmlDocument::parse (file);
                              const auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
                              if (! tree.hasType (genisys::MidiMap::treeType))
                              {
                                  help.show ("COULDN'T LOAD|" + file.getFileName() + " isn't a Genisys controller map.");
                                  return;
                              }
                              genisys.midiMap.fromValueTree (tree);
                              help.show ("LOADED|" + file.getFileName());
                          });
}

void GenisysEditor::saveControllerMap()
{
    chooser = std::make_unique<juce::FileChooser> ("Save the controller map", juce::File(), "*.genisysmap");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension (".genisysmap");
                              auto xml = genisys.midiMap.toValueTree().createXml();
                              const bool ok = xml != nullptr && xml->writeTo (file);
                              help.show (ok ? "SAVED|" + file.getFileName() : "COULDN'T SAVE|" + file.getFileName());
                          });
}

void GenisysEditor::buildFxPage (Page& page)
{
    page.add<MenuWindow> ({ 0, 0, 316, 372 }, "CHORUS");
    auto& chorusOn = page.add<LedToggle> ({ 14, 32, 120, 16 }, param ("chorus_on"), "ON");
    setHelp (chorusOn, "CHORUS", "Thickens the sound with a gentle pitch wobble.");
    page.addAt<RetroKnob> ({ 14, 64 }, param ("chorus_rate"), "RATE").withFormatter (hz).withHelp ("RATE", "Wobble speed.");
    page.addAt<RetroKnob> ({ 94, 64 }, param ("chorus_depth"), "DEPTH").withHelp ("DEPTH", "Wobble amount.");
    page.addAt<RetroKnob> ({ 174, 64 }, param ("chorus_mix"), "MIX").withHelp ("MIX", "How much chorus is blended in.");

    page.add<MenuWindow> ({ 326, 0, 316, 372 }, "ECHO");
    auto& echoOn = page.add<LedToggle> ({ 340, 32, 100, 16 }, param ("echo_on"), "ON");
    setHelp (echoOn, "ECHO", "Repeats of the sound. Each repeat gets a little darker, like tape.");
    auto& sync = page.add<LedToggle> ({ 440, 32, 180, 16 }, param ("echo_sync"), "SYNC TO TEMPO");
    setHelp (sync, "SYNC TO TEMPO", "On: echo time follows your song's tempo (the TIME knob). Off: set it in milliseconds (MS).");
    page.addAt<RetroKnob> ({ 340, 64 }, param ("echo_division"), "TIME").withHelp ("TIME", "Echo length as a note value, when synced.");
    page.addAt<RetroKnob> ({ 420, 64 }, param ("echo_time"), "MS").withHelp ("MS", "Echo length in milliseconds, when not synced.");
    page.addAt<RetroKnob> ({ 340, 164 }, param ("echo_feedback"), "FEEDBACK").withHelp ("FEEDBACK", "How many repeats.");
    page.addAt<RetroKnob> ({ 420, 164 }, param ("echo_mix"), "MIX").withHelp ("MIX", "How loud the echoes are.");
    auto& ping = page.add<LedToggle> ({ 340, 280, 200, 16 }, param ("echo_pingpong"), "PING-PONG");
    setHelp (ping, "PING-PONG", "Echoes bounce between left and right.");

    page.add<MenuWindow> ({ 652, 0, 316, 372 }, "REVERB");
    auto& revOn = page.add<LedToggle> ({ 666, 32, 120, 16 }, param ("reverb_on"), "ON");
    setHelp (revOn, "REVERB", "Places the sound in a room or hall.");
    page.addAt<RetroKnob> ({ 666, 64 }, param ("reverb_size"), "SIZE").withHelp ("SIZE", "Room size.");
    page.addAt<RetroKnob> ({ 746, 64 }, param ("reverb_damping"), "DAMP").withHelp ("DAMPING", "Darker tail.");
    page.addAt<RetroKnob> ({ 666, 164 }, param ("reverb_width"), "WIDTH").withHelp ("WIDTH", "Stereo width.");
    page.addAt<RetroKnob> ({ 746, 164 }, param ("reverb_mix"), "MIX").withHelp ("MIX", "How much reverb.");
}

void GenisysEditor::buildModPage (Page& page)
{
    page.add<MenuWindow> ({ 0, 0, 478, 372 }, "CHIP LFO");
    auto& lfo = page.add<LedToggle> ({ 14, 32, 160, 16 }, param ("lfo_enable"), "LFO ON");
    setHelp (lfo, "CHIP LFO", "The YM2612's own LFO: shared by all voices, with 8 fixed speeds.");
    page.addAt<RetroKnob> ({ 20, 64 }, param ("lfo_rate"), "RATE").withHelp ("LFO RATE", "Speed of the chip LFO.");
    page.addAt<RetroKnob> ({ 100, 64 }, param ("ams"), "TREMOLO").withHelp ("TREMOLO", "Volume pulsing on operators with TREM switched on (OPERATORS tab).");
    page.addAt<RetroKnob> ({ 180, 64 }, param ("pms"), "VIBRATO").withHelp ("CHIP VIBRATO", "Pitch wobble from the chip LFO.");
    page.add<PixelText> ({ 14, 180, 450, 100 },
                         "Tip: the chip LFO is the authentic Genesis vibrato. The MOD WHEEL section uses smooth software vibrato instead, like game sound drivers did.",
                         1.2f, theme::textDim, true);
    auto& crt = page.add<LedToggle> ({ 14, 320, 220, 16 }, "CRT SCANLINES");
    crt.setOn (canvas.scanlines);
    setHelp (crt, "CRT SCANLINES", "Display only: the faint TV scanlines over the panel.");
    crt.onChange = [this] (bool on) { canvas.scanlines = on; canvas.repaint(); };

    page.add<MenuWindow> ({ 490, 0, 478, 372 }, "MOD WHEEL & BEND");
    page.addAt<RetroKnob> ({ 510, 32 }, param ("vibrato_depth"), "DEPTH").withHelp ("VIBRATO DEPTH", "How far the pitch wobbles with the mod wheel all the way up.");
    page.addAt<RetroKnob> ({ 590, 32 }, param ("vibrato_rate"), "SPEED").withHelp ("VIBRATO SPEED", "How fast the vibrato wobbles.");
    page.addAt<RetroKnob> ({ 670, 32 }, param ("bend_range"), "BEND").withHelp ("BEND RANGE", "How far the pitch wheel bends, in semitones.");
    page.addAt<RetroKnob> ({ 750, 32 }, param ("octave"), "OCTAVE").withHelp ("OCTAVE", "Shifts everything you play up or down.");
}

// ---------------------------------------------------------------------------
void GenisysEditor::timerCallback()
{
    cartridge.refresh();
    refreshMidiMap();

    // Operator cards: carrier/modulator labels follow the algorithm.
    const int algorithm = (int) std::lround (genisys.state.getRawParameterValue ("algorithm")->load());
    const uint8_t carriers = genisys_carrier_mask (algorithm);
    for (int op = 0; op < 4; ++op)
    {
        cards[(size_t) op]->setCarrier (((carriers >> op) & 1) != 0);
        cards[(size_t) op]->refreshEnvelope();
    }

    const int octave = (int) std::lround (genisys.state.getRawParameterValue ("octave")->load());
    if (octave != octaveReadout.octave)
    {
        octaveReadout.octave = octave;
        octaveReadout.repaint();
    }
}

void GenisysEditor::mouseEnter (const juce::MouseEvent& e) { mouseMove (e); }

void GenisysEditor::mouseMove (const juce::MouseEvent& e)
{
    if (learnMode || genisys.midiMap.learningParamId().isNotEmpty())
        return; // keep the learn instructions on screen
    auto text = getHelp (*e.eventComponent);
    const auto id = getMappableParamId (*e.eventComponent);
    if (text.isNotEmpty() && id.isNotEmpty())
    {
        const int cc = genisys.midiMap.ccFor (id);
        if (cc >= 0)
            text << " [MIDI: CC " << cc << "]";
    }
    if (text.isNotEmpty())
        help.show (text);
}

void GenisysEditor::setKeyboardToDrums (bool drums)
{
    keyboard.setMidiChannel (drums ? 10 : 1);
    keyboard.setMidiChannelsToDisplay (drums ? (1 << 9) : 0xFFFF);
    keyboard.setKeyPressBaseOctave (drums ? 3 : 5); // computer keys start at C2 for drums, C4 for synth
    if (drums)
    {
        auto& drumsOn = param ("drums_on");
        if (drumsOn.getValue() < 0.5f)
            drumsOn.setValueNotifyingHost (1.0f);
        help.show ("KEYS PLAY DRUMS|A KICK  W RIM  S SNARE  E CLAP  D SNARE 2  T CL HAT  Y PEDAL HAT  U OPEN HAT  O CRASH  P RIDE  F G H J K L TOMS");
    }
}

void GenisysEditor::shiftOctave (int delta)
{
    auto& octave = param ("octave");
    const float current = octave.convertFrom0to1 (octave.getValue());
    octave.beginChangeGesture();
    octave.setValueNotifyingHost (octave.convertTo0to1 (current + (float) delta));
    octave.endChangeGesture();
    timerCallback();
}

bool GenisysEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && (learnMode || genisys.midiMap.learningParamId().isNotEmpty()))
    {
        if (learnMode)
            setLearnMode (false);
        else
        {
            genisys.midiMap.stopLearning();
            help.show ("MIDI LEARN|Cancelled.");
        }
        return true;
    }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        genisys.stepPreset (key == juce::KeyPress::rightKey ? 1 : -1);
        cartridge.refresh();
        return true;
    }
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    if (c == 'z') { shiftOctave (-1); return true; }
    if (c == 'x') { shiftOctave (1); return true; }
    return false;
}

void GenisysEditor::showLoadMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Load a preset file (.genisyspreset)...");
    menu.addItem (2, "Import a Genesis FM patch (.tfi / .vgi / .dmp)...");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadButton),
                        [this] (int result)
                        {
                            if (result == 1) loadPresetFile();
                            if (result == 2) loadPatch();
                        });
}

void GenisysEditor::showSaveMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Save as one of my presets...");
    menu.addItem (2, "Export the FM patch (.tfi)...");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&saveButton),
                        [this] (int result)
                        {
                            if (result == 1) saveUserPreset();
                            if (result == 2) savePatch();
                        });
}

void GenisysEditor::saveUserPreset()
{
    const auto info = genisys.getCurrentPresetInfo();
    auto* window = new juce::AlertWindow ("SAVE PRESET", "Name your sound. It will appear under MY PRESETS in the cartridge menu.",
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", info.user ? info.name : info.name + " (mine)", "Name");
    window->addTextEditor ("category", info.user ? info.category : juce::String ("User"), "Category");
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<GenisysEditor> safeThis (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, window] (int result)
    {
        if (safeThis == nullptr || result != 1)
            return;
        const auto name = window->getTextEditorContents ("name").trim();
        const auto category = window->getTextEditorContents ("category").trim();
        if (name.isEmpty())
        {
            safeThis->help.show ("SAVE PRESET|Give the preset a name first.");
            return;
        }
        auto& p = safeThis->genisys;
        const auto file = p.presetLibrary.save (p.state, name, category);
        if (file == juce::File())
        {
            safeThis->help.show ("SAVE PRESET|Couldn't save the preset.");
            return;
        }
        p.loadUserPreset (file); // the cartridge now shows it
        safeThis->help.show ("SAVED!|" + name + " is in MY PRESETS. Click the star to make it a favourite.");
    }), true);
}

void GenisysEditor::loadPresetFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a Genisys preset", juce::File(), "*.genisyspreset");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              help.show (genisys.loadUserPreset (file) ? "LOADED|" + file.getFileNameWithoutExtension()
                                                                       : "COULDN'T LOAD|" + file.getFileName() + " isn't a Genisys preset.");
                          });
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
                              help.show (error.isEmpty() ? "LOADED|" + file.getFileName() : "COULDN'T LOAD|" + error);
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
                              help.show (error.isEmpty() ? "SAVED|" + file.getFileName() : "COULDN'T SAVE|" + error);
                          });
}

void GenisysEditor::toggleCompare()
{
    // Remember the current sound in this slot, then switch to the other one
    // (which starts as a copy, so the first press changes nothing audible).
    compareSlots[compareSlot] = genisys.state.copyState();
    compareSlot ^= 1;
    if (! compareSlots[compareSlot].isValid())
        compareSlots[compareSlot] = genisys.state.copyState();
    genisys.state.replaceState (compareSlots[compareSlot].createCopy());
    compareButton.setCaption (compareSlot == 0 ? "A/B: A" : "A/B: B");
    help.show (juce::String ("COMPARE|Now editing version ") + (compareSlot == 0 ? "A" : "B")
               + ". Press C again to flip back. Each version keeps its own edits.");
}
