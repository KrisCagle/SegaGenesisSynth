#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

// The FM arpeggiator: turns held notes into a repeating pattern locked to
// the song's tempo, like the fast arpeggios of 16-bit soundtracks.
//
// It rewrites the MIDI before the synth sees it: note-ons and note-offs
// become the pattern's notes; everything else (CCs, pitch bend, drum notes)
// passes straight through. Audio thread only; no allocation after prepare().
namespace genisys
{
    class Arpeggiator
    {
    public:
        enum class Mode { up, down, upDown, random, asPlayed };

        struct Settings
        {
            bool on = false;
            Mode mode = Mode::up;
            double beatsPerStep = 0.25; // 1/16 note
            int octaves = 1;            // 1-4
            float gate = 0.5f;          // fraction of a step each note lasts
            bool latch = false;         // keep playing after the keys are let go
        };

        void prepare (double sampleRate);
        void reset();

        // `passThrough` returns true for messages the arpeggiator must leave
        // alone (e.g. notes meant for the drum kit).
        template <typename PassThrough>
        void process (juce::MidiBuffer& midi, int numSamples, const Settings& s, double bpm, PassThrough passThrough);

        // The notes the pattern cycles through (for tests and display).
        const std::vector<int>& pattern() const { return steps; }

    private:
        struct Held { int note, velocity; };

        void rebuildPattern (const Settings&);
        void startNote (juce::MidiBuffer& out, int position, const Settings&);
        void stopNote (juce::MidiBuffer& out, int position);
        void addHeld (int note, int velocity);
        void removeHeld (int note);

        double sampleRate = 44100.0;
        std::vector<Held> held;   // keys down, in the order played
        std::vector<Held> latched;
        std::vector<int> steps;   // the pattern
        int stepIndex = 0, direction = 1;
        int sounding = -1;        // note currently on, -1 = none
        int channel = 1, velocity = 100;
        double untilNextStep = 0.0, untilNoteOff = 0.0; // in samples
        bool running = false, wasOn = false, offPending = false;
        juce::Random random;
        juce::MidiBuffer output;
    };

    template <typename PassThrough>
    void Arpeggiator::process (juce::MidiBuffer& midi, int numSamples, const Settings& s, double bpm, PassThrough passThrough)
    {
        if (! s.on)
        {
            if (wasOn)
            {
                // Switched off: silence the pattern, then let held keys sound.
                juce::MidiBuffer fresh;
                stopNote (fresh, 0);
                for (const auto& h : held)
                    fresh.addEvent (juce::MidiMessage::noteOn (channel, h.note, (juce::uint8) h.velocity), 0);
                fresh.addEvents (midi, 0, numSamples, 0);
                midi.swapWith (fresh);
                reset();
            }
            wasOn = false;
            // Track keys anyway, so switching on mid-chord starts at once.
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                if (passThrough (msg)) continue;
                if (msg.isNoteOn()) addHeld (msg.getNoteNumber(), msg.getVelocity());
                else if (msg.isNoteOff()) removeHeld (msg.getNoteNumber());
            }
            return;
        }

        if (! wasOn)
        {
            // Just switched on: turn off keys that were sounding normally.
            wasOn = true;
            output.clear();
            for (const auto& h : held)
                output.addEvent (juce::MidiMessage::noteOff (channel, h.note), 0);
            rebuildPattern (s);
            running = ! steps.empty();
            untilNextStep = 0.0;
        }
        else
        {
            output.clear();
        }

        // Latch switched off with no keys down: the latched chord stops.
        if (! s.latch && ! latched.empty())
        {
            latched.clear();
            rebuildPattern (s);
            if (steps.empty())
            {
                stopNote (output, 0);
                offPending = false;
                running = false;
            }
        }

        const double stepSamples = juce::jmax (1.0, 60.0 / juce::jlimit (20.0, 400.0, bpm) * s.beatsPerStep * sampleRate);
        auto it = midi.begin();
        int pos = 0;

        for (;;)
        {
            // Next thing to happen: an incoming event, a note-off or a step.
            const int eventPos = it != midi.end() ? juce::jlimit (pos, numSamples - 1, (*it).samplePosition) : numSamples;
            const int stepPos = running ? pos + juce::jmax (0, (int) std::ceil (untilNextStep)) : numSamples;
            const int offPos = offPending ? pos + juce::jmax (0, (int) std::ceil (untilNoteOff)) : numSamples;
            const int next = juce::jmin (eventPos, stepPos, offPos);

            const int advance = juce::jmin (next, numSamples) - pos;
            untilNextStep -= advance;
            untilNoteOff -= advance;
            pos += advance;
            if (next >= numSamples)
                break;

            if (offPending && untilNoteOff <= 0.0)
            {
                stopNote (output, pos);
                offPending = false;
                continue;
            }

            if (it != midi.end() && eventPos == pos)
            {
                const auto msg = (*it).getMessage();
                ++it;
                if (passThrough (msg) || ! (msg.isNoteOn() || msg.isNoteOff()))
                {
                    output.addEvent (msg, pos);
                    continue;
                }
                channel = msg.getChannel();
                if (msg.isNoteOn())
                {
                    if (s.latch && held.empty())
                        latched.clear(); // a new chord replaces the latched one
                    addHeld (msg.getNoteNumber(), msg.getVelocity());
                    velocity = msg.getVelocity();
                    if (s.latch)
                        latched.push_back ({ msg.getNoteNumber(), msg.getVelocity() });
                }
                else
                {
                    removeHeld (msg.getNoteNumber());
                }
                rebuildPattern (s);
                if (steps.empty())
                {
                    stopNote (output, pos);
                    offPending = false;
                    running = false;
                }
                else if (! running)
                {
                    running = true; // first key: play straight away
                    stepIndex = 0;
                    direction = 1;
                    untilNextStep = 0.0;
                }
                continue;
            }

            if (running && untilNextStep <= 0.0)
            {
                startNote (output, pos, s);
                untilNextStep += stepSamples;
                // At 100% gate the next note-on replaces this one.
                offPending = s.gate < 0.999f;
                untilNoteOff = juce::jmax (1.0, stepSamples * juce::jlimit (0.05f, 1.0f, s.gate));
            }
        }

        // Events at or after the block end (shouldn't happen) pass through.
        for (; it != midi.end(); ++it)
            output.addEvent ((*it).getMessage(), numSamples - 1);

        midi.swapWith (output);
    }
}
