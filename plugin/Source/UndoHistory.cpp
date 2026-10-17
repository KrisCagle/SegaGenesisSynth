#include "UndoHistory.h"

namespace genisys
{
UndoHistory::UndoHistory (juce::AudioProcessor& processor)
{
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            params.push_back (ranged);
    settled = pending = read();
}

std::vector<float> UndoHistory::read() const
{
    std::vector<float> values;
    values.reserve (params.size());
    for (auto* p : params)
        values.push_back (p->getValue());
    return values;
}

void UndoHistory::apply (const std::vector<float>& values)
{
    for (size_t i = 0; i < params.size() && i < values.size(); ++i)
    {
        if (params[i]->getValue() == values[i])
            continue;
        params[i]->beginChangeGesture();
        params[i]->setValueNotifyingHost (values[i]);
        params[i]->endChangeGesture();
    }
    settled = pending = read();
    stablePolls = 0;
}

void UndoHistory::poll()
{
    const auto now = read();
    if (now != pending)
    {
        pending = now; // still moving
        stablePolls = 0;
        return;
    }
    if (pending != settled && ++stablePolls >= settlePolls)
        commitPending();
}

void UndoHistory::commitPending()
{
    const auto now = read();
    if (now == settled)
        return;
    undoStack.push_back (settled);
    if (undoStack.size() > maxSteps)
        undoStack.erase (undoStack.begin());
    settled = pending = now;
    redoStack.clear();
}

bool UndoHistory::canUndo() const
{
    return ! undoStack.empty() || read() != settled;
}

bool UndoHistory::undo()
{
    commitPending(); // a change still settling counts as the latest step
    if (undoStack.empty())
        return false;
    redoStack.push_back (settled);
    const auto target = undoStack.back();
    undoStack.pop_back();
    apply (target);
    return true;
}

bool UndoHistory::redo()
{
    if (redoStack.empty())
        return false;
    undoStack.push_back (settled);
    const auto target = redoStack.back();
    redoStack.pop_back();
    apply (target);
    return true;
}
}
