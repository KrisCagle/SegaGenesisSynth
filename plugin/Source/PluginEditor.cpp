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
      scope (p),
      wheels (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
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
    setHelp (loadButton, "A - LOAD", "Load a Genesis FM patch from TFM Music Maker (.TFI), VGM Music Maker (.VGI) or DefleMask (.DMP).");
    setHelp (saveButton, "B - SAVE", "Save this sound as a .TFI patch that other Genesis tools can open.");
    setHelp (compareButton, "C - COMPARE", "Flip between two versions of your sound (A and B) to compare edits.");
    loadButton.onClick = [this] { loadPatch(); };
    saveButton.onClick = [this] { savePatch(); };
    compareButton.onClick = [this] { toggleCompare(); };

    // Tabs and pages.
    canvas.addAndMakeVisible (tabs);
    tabs.setBounds (16, 80, 968, 30);
    tabs.helpTexts = { "The big picture: algorithm, quick sound shaping and how notes play.",
                       "Deep editing of the 4 FM operators.",
                       "The square-wave chip: layers, arpeggios and noise.",
                       "The 8-bit drum kit on MIDI channel 10.",
                       "Chorus, echo and reverb.",
                       "Vibrato, tremolo, pitch bend and display options." };
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
    showPage (0);

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
    setHelp (keyboard, "KEYBOARD", "Click to play, or use A W S E D F T G Y H U J K on your computer keyboard. Z / X change octave.");
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
    canvas.removeMouseListener (this);
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
    for (size_t i = 0; i < pages.size(); ++i)
        pages[i].setVisible ((int) i == index);
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
    page.add<MenuWindow> ({ 0, 0, 700, 372 }, "DRUM KIT - MIDI CH 10");
    page.add<DrumPads> ({ 12, 32, 674, 320 }, genisys);

    page.add<MenuWindow> ({ 710, 0, 258, 372 }, "KIT");
    auto& on = page.add<LedToggle> ({ 724, 32, 220, 16 }, param ("drums_on"), "DRUMS ON");
    setHelp (on, "DRUMS ON", "Gives FM channel 6 to the drum sampler, like the games did. FM then has 5 voices.");
    page.addAt<RetroKnob> ({ 724, 60 }, param ("drum_level"), "LEVEL").withHelp ("DRUM LEVEL", "Volume of the whole kit.");
    auto& keys = page.add<LedToggle> ({ 724, 172, 230, 16 }, "KEYS PLAY DRUMS");
    setHelp (keys, "KEYS PLAY DRUMS", "Sends the on-screen and computer keyboard to the drum kit instead of the synth.");
    keys.onChange = [this] (bool drums) { setKeyboardToDrums (drums); };
    page.add<PixelText> ({ 724, 204, 230, 150 },
                         "Yellow pads: 8-bit samples through the FM chip's DAC. Cyan pads: PSG noise. Play from any MIDI channel 10.",
                         1.15f, theme::text, true);
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
    const auto text = getHelp (*e.eventComponent);
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
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    if (c == 'z') { shiftOctave (-1); return true; }
    if (c == 'x') { shiftOctave (1); return true; }
    return false;
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
