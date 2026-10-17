#pragma once

#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

// Undo / redo for every parameter, however it was changed (mouse, MIDI
// knob, preset load, patch import).
//
// Instead of recording each tiny knob movement, it watches the parameters
// and records a step once they have stopped moving for a moment, so one
// knob turn is one undo step. Message thread only.
namespace genisys
{
    class UndoHistory
    {
    public:
        explicit UndoHistory (juce::AudioProcessor&);

        // Call regularly (e.g. 15 times a second).
        void poll();

        bool canUndo() const;
        bool canRedo() const { return ! redoStack.empty(); }
        bool undo(); // false if there was nothing to undo
        bool redo();

        static constexpr int settlePolls = 4;  // ~0.25 s at 15 polls/s
        static constexpr size_t maxSteps = 100;

    private:
        std::vector<float> read() const;
        void apply (const std::vector<float>&);
        void commitPending();

        std::vector<juce::RangedAudioParameter*> params;
        std::vector<std::vector<float>> undoStack, redoStack;
        std::vector<float> settled, pending;
        int stablePolls = 0;
    };
}
