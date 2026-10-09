#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Panels.h"
#include "ui/Widgets.h"

// The Genisys editor: a 16-bit game-menu style front panel.
//
// Everything is laid out on a fixed 1000 x 690 "design" canvas and scaled as
// a whole when the window is resized, so the pixel art stays in proportion.
class GenisysEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit GenisysEditor (GenisysProcessor&);
    ~GenisysEditor() override;

    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Switches to a tab (0 = VOICE .. 5 = MOD), as clicking it would.
    void selectTab (int index);
    static constexpr int numTabs = 6;
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
    void showPage (int index);
    void setKeyboardToDrums (bool drums);
    void shiftOctave (int delta);
    void loadPatch();
    void savePatch();
    void toggleCompare();
    juce::RangedAudioParameter& param (const char* id);

    GenisysProcessor& genisys;

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
    genisys::ui::TabBar tabs { { "VOICE", "OPERATORS", "PSG", "DRUMS", "FX", "MOD" } };
    std::array<Page, 6> pages;
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
