#pragma once

#include <vector>

#include "../MidiMap.h"
#include "Widgets.h"

namespace genisys::ui
{
    // Drawn over a page while MIDI Learn is on: outlines every control a
    // hardware knob can be linked to, tags the linked ones with their CC,
    // and flashes the one waiting for a knob to move.
    class LearnOverlay : public juce::Component
    {
    public:
        explicit LearnOverlay (MidiMap&);
        void setPage (juce::Component* pageToCover) { page = pageToCover; repaint(); }
        void blink() { blinkOn = ! blinkOn; repaint(); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        std::function<void (const juce::String& paramId)> onPick;   // click
        std::function<void (const juce::String& paramId)> onForget; // right-click

    private:
        struct Target
        {
            juce::String paramId;
            juce::Rectangle<int> area;
        };
        std::vector<Target> findTargets() const;
        void collect (juce::Component&, std::vector<Target>&) const;

        MidiMap& map;
        juce::Component* page = nullptr;
        bool blinkOn = true;
    };

    // The list of links on the MIDI page: CC, control, and the part of the
    // control's range the knob covers (drag MIN / MAX; MIN above MAX turns
    // the knob around).
    class MappingList : public juce::Component
    {
    public:
        explicit MappingList (MidiMap&);
        void refresh();
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        static constexpr int headerHeight = 18, rowHeight = 22;

    private:
        enum Column { ccColumn, nameColumn, minColumn, maxColumn, forgetColumn, none };
        juce::Rectangle<float> cell (int visibleRow, Column) const;
        int rowAt (juce::Point<float>) const;
        Column columnAt (juce::Point<float>) const;
        int visibleRows() const;

        MidiMap& map;
        std::vector<MidiMap::Mapping> rows;
        int scroll = 0;
        int dragRow = -1;
        Column dragColumn = none;
        float dragStart = 0.0f;
    };
}
