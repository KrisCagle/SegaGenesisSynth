#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "UndoHistory.h"
#include "ui/MidiPanels.h"
#include "ui/Panels.h"
#include "ui/Widgets.h"

// The Genisys editor: a 16-bit game-menu style front panel.
//
// Everything is laid out on a fixed 1000 x 690 "design" canvas and scaled as
// a whole when the window is resized, so the pixel art stays in proportion.
class GenisysEditor final : public juce::AudioProcessorEditor,
                            public genisys::ui::ParameterMenuHandler,
                            private juce::Timer
{
public:
    explicit GenisysEditor (GenisysProcessor&);
    ~GenisysEditor() override;

    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Switches to a tab (0 = VOICE .. 7 = MIDI), as clicking it would.
    void selectTab (int index);
    static constexpr int numTabs = 8;

    // MIDI Learn mode: outlines every mappable control; click one, then
    // move a hardware knob. (Also used by the UI snapshot tool.)
    void setLearnMode (bool on);

    void showParameterMenu (juce::Component& source, juce::RangedAudioParameter&) override;
    void showActionMenu (juce::Component& source, const juce::String& actionId) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;

    // A page of the tabbed panel: owns whatever is put on it.
    class Page : public juce::Component
    {
    public:
        template <typename T, typename... Args>
        T& add (juce::Rectangle<int> bounds, Args&&... args)
        {
            auto item = std::make_unique<T> (std::forward<Args> (args)...);
            auto& ref = *item;
            ref.setBounds (bounds);
            addAndMakeVisible (ref);
            items.push_back (std::move (item));
            return ref;
        }

        template <typename T, typename... Args>
        T& addAt (juce::Point<int> topLeft, Args&&... args) // keeps the item's own size
        {
            auto item = std::make_unique<T> (std::forward<Args> (args)...);
            auto& ref = *item;
            ref.setTopLeftPosition (topLeft);
            addAndMakeVisible (ref);
            items.push_back (std::move (item));
            return ref;
        }

    private:
        std::vector<std::unique_ptr<juce::Component>> items;
    };

private:
    void timerCallback() override;
    void buildVoicePage (Page&);
    void buildOperatorsPage (Page&);
    void buildPsgPage (Page&);
    void buildDrumsPage (Page&);
    void buildFxPage (Page&);
    void buildModPage (Page&);
    void buildArpPage (Page&);
    void buildMidiPage (Page&);
    void setWindowScale (float scale);
    void doUndo (bool redo);
    void refreshMidiMap();
    void loadControllerMap();
    void saveControllerMap();
    void showPage (int index);
    void setKeyboardToDrums (bool drums);
    void shiftOctave (int delta);
    void loadPatch();
    void savePatch();
    void showLoadMenu();
    void showSaveMenu();
    void saveUserPreset();
    void loadPresetFile();
    void toggleCompare();
    juce::RangedAudioParameter& param (const char* id);

    GenisysProcessor& genisys;
    juce::LookAndFeel_V4 lookAndFeel; // themes pop-up menus; outlives every child

    // The design canvas (scaled as a whole) with chassis, scanlines etc.
    class Canvas : public juce::Component
    {
    public:
        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        bool scanlines = true;
    };
    Canvas canvas;

    genisys::ui::Logo logo;
    genisys::ui::PresetCartridge cartridge;
    genisys::ui::PadButton loadButton { "A", "LOAD" }, saveButton { "B", "SAVE" }, compareButton { "C", "A/B: A" };
    genisys::ui::TabBar tabs { { "VOICE", "OPERATORS", "PSG", "DRUMS", "FX", "MOD", "ARP", "MIDI" } };
    std::array<Page, numTabs> pages;
    int currentPage = 0;

    genisys::ui::PixelButton learnButton { "LEARN" };
    genisys::ui::PixelButton undoButton { "UNDO" }, redoButton { "REDO" };
    genisys::UndoHistory history;
    std::array<genisys::ui::PixelButton*, 4> sizeButtons {};
    genisys::ui::LearnOverlay learnOverlay;
    genisys::ui::MappingList* mappingList = nullptr;
    genisys::ui::LedToggle* pickupToggle = nullptr;
    genisys::ui::LedToggle* programChangeToggle = nullptr;
    bool learnMode = false;
    juce::String lastLearning;
    juce::uint32 shownMapVersion = 0;
    int blinkTicks = 0;
    std::array<genisys::ui::OperatorCard*, 4> cards {};

    genisys::ui::HelpBox help;
    genisys::ui::ScopeScreen scope;
    genisys::ui::Wheels wheels;
    genisys::ui::PixelButton octaveUp { "OCT+" }, octaveDown { "OCT-" };
    class OctaveReadout : public juce::Component
    {
    public:
        void paint (juce::Graphics&) override;
        int octave = 0;
    } octaveReadout;
    juce::MidiKeyboardComponent keyboard;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::ValueTree compareSlots[2];
    int compareSlot = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenisysEditor)
};
