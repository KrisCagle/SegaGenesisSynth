#include "Arpeggiator.h"

#include <algorithm>

namespace genisys
{
void Arpeggiator::prepare (double rate)
{
    sampleRate = rate;
    held.reserve (128);
    latched.reserve (128);
    steps.reserve (128 * 4 * 2);
    output.ensureSize (2048);
    reset();
}

void Arpeggiator::reset()
{
    latched.clear();
    steps.clear();
    stepIndex = 0;
    direction = 1;
    sounding = -1;
    running = false;
    untilNextStep = 0.0;
    untilNoteOff = 0.0;
    offPending = false;
}

void Arpeggiator::addHeld (int note, int vel)
{
    removeHeld (note);
    held.push_back ({ note, vel });
}

void Arpeggiator::removeHeld (int note)
{
    held.erase (std::remove_if (held.begin(), held.end(), [note] (const Held& h) { return h.note == note; }), held.end());
}

void Arpeggiator::rebuildPattern (const Settings& s)
{
    const auto& source = (s.latch && ! latched.empty()) ? latched : held;
    steps.clear();
    if (source.empty())
        return;

    // One octave of notes, in the chosen order.
    std::vector<int> base;
    base.reserve (source.size());
    for (const auto& h : source)
        base.push_back (h.note);
    if (s.mode != Mode::asPlayed)
    {
        std::sort (base.begin(), base.end());
        base.erase (std::unique (base.begin(), base.end()), base.end());
    }

    const int octaves = juce::jlimit (1, 4, s.octaves);
    for (int o = 0; o < octaves; ++o)
        for (int n : base)
            if (n + 12 * o <= 127)
                steps.push_back (n + 12 * o);

    if (s.mode == Mode::down)
        std::reverse (steps.begin(), steps.end());
    if (stepIndex >= (int) steps.size())
        stepIndex = 0;
}

void Arpeggiator::startNote (juce::MidiBuffer& out, int position, const Settings& s)
{
    if (steps.empty())
        return;
    const int count = (int) steps.size();
    int index = 0;
    switch (s.mode)
    {
        case Mode::random:
            index = random.nextInt (count);
            break;
        case Mode::upDown:
            // 1 2 3 2 1 2 3...: the ends aren't repeated.
            if (count > 1)
            {
                if (stepIndex >= count) { stepIndex = count - 2; direction = -1; }
                if (stepIndex < 0) { stepIndex = 1; direction = 1; }
            }
            index = juce::jlimit (0, count - 1, stepIndex);
            stepIndex += count > 1 ? direction : 0;
            break;
        case Mode::up:
        case Mode::down:
        case Mode::asPlayed:
        default:
            index = stepIndex % count;
            stepIndex = (stepIndex + 1) % count;
            break;
    }
    stopNote (out, position);
    sounding = steps[(size_t) index];
    out.addEvent (juce::MidiMessage::noteOn (channel, sounding, (juce::uint8) juce::jlimit (1, 127, velocity)), position);
}

void Arpeggiator::stopNote (juce::MidiBuffer& out, int position)
{
    if (sounding >= 0)
        out.addEvent (juce::MidiMessage::noteOff (channel, sounding), position);
    sounding = -1;
}
}
