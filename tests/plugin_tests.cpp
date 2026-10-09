// Drives the Genisys AudioProcessor the way a DAW would (prepare, MIDI in,
// processBlock, save/restore state) without needing a DAW.

#include <cmath>
#include <cstdio>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"
#include "genisys_presets.h"

namespace
{
    int failures = 0;

    void check (bool condition, const char* message)
    {
        std::printf ("%s: %s\n", condition ? "PASS" : "FAIL", message);
        if (! condition)
            ++failures;
    }

    constexpr double kRate = 48000.0;
    constexpr int kBlock = 512;

    std::unique_ptr<GenisysProcessor> makeProcessor()
    {
        auto p = std::make_unique<GenisysProcessor>();
        p->setPlayConfigDetails (0, 2, kRate, kBlock);
        p->prepareToPlay (kRate, kBlock);
        return p;
    }

    float rms (const juce::AudioBuffer<float>& b, int start, int count)
    {
        double sum = 0.0;
        for (int i = start; i < start + count; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);
        return (float) std::sqrt (sum / count);
    }

    float peak (const juce::AudioBuffer<float>& b, int start, int count)
    {
        return b.getMagnitude (0, start, count);
    }

    void process (GenisysProcessor& p, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        buffer.clear();
        p.processBlock (buffer, midi);
        midi.clear();
    }

    int intParam (GenisysProcessor& p, const char* id)
    {
        return (int) std::lround (p.state.getRawParameterValue (id)->load());
    }

    void setIntParam (GenisysProcessor& p, const char* id, int value)
    {
        auto* param = p.state.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 ((float) value));
    }

    void testBasics()
    {
        auto p = makeProcessor();
        check (p->getName() == "Genisys", "plugin reports its name");
        check (p->acceptsMidi() && ! p->producesMidi(), "plugin is a MIDI-in instrument");
        check (p->getNumPrograms() == genisys_preset_count(), "factory presets are exposed as programs");
    }

    void testSilenceThenNote()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        process (*p, buffer, midi);
        check (peak (buffer, 0, kBlock) == 0.0f, "no notes -> exact silence");

        midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
        process (*p, buffer, midi);
        process (*p, buffer, midi);
        check (rms (buffer, 0, kBlock) > 0.01f, "a note-on produces sound");
    }

    void testSampleAccurateNoteOn()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 256);
        process (*p, buffer, midi);
        check (peak (buffer, 0, 256) == 0.0f, "nothing sounds before the note's sample position");
        check (rms (buffer, 320, kBlock - 320) > 0.01f, "sound starts at the note's sample position");
    }

    void testNoteOffReleases()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        process (*p, buffer, midi);
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        for (int i = 0; i < (int) (kRate * 3.0) / kBlock; ++i)
            process (*p, buffer, midi);
        check (peak (buffer, 0, kBlock) < 1.0e-4f, "after note-off the release fades to silence");
    }

    void testParameterChangesSound()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
        process (*p, buffer, midi);
        process (*p, buffer, midi);
        const float before = rms (buffer, 0, kBlock);

        p->state.getParameter ("master_gain")->setValueNotifyingHost (
            p->state.getParameter ("master_gain")->convertTo0to1 (-24.0f));
        for (int i = 0; i < 10; ++i) // let the 20 ms gain ramp finish
            process (*p, buffer, midi);
        const float after = rms (buffer, 0, kBlock);

        check (after < before * 0.1f, "lowering Master Volume by 24 dB lowers the output");
    }

    void testStateRoundTrip()
    {
        auto a = makeProcessor();
        setIntParam (*a, "algorithm", 5);
        setIntParam (*a, "op2_tl", 42);
        setIntParam (*a, "velocity_sens", 77);

        juce::MemoryBlock saved;
        a->getStateInformation (saved);

        auto b = makeProcessor();
        b->setStateInformation (saved.getData(), (int) saved.getSize());
        check (intParam (*b, "algorithm") == 5 && intParam (*b, "op2_tl") == 42 && intParam (*b, "velocity_sens") == 77,
               "saved state restores every parameter (what a DAW project does on reopen)");
    }

    void testProgramChange()
    {
        auto p = makeProcessor();
        const int index = 3;
        const GenisysPatch preset = genisys_preset_patch (index);
        p->setCurrentProgram (index);
        check (p->getCurrentProgram() == index, "selecting a program updates the current program");
        check (intParam (*p, "algorithm") == preset.algorithm && intParam (*p, "op4_tl") == preset.op[3].tl,
               "selecting a program loads that preset's parameters");
    }

    void testMonoOutput()
    {
        auto p = std::make_unique<GenisysProcessor>();
        juce::AudioProcessor::BusesLayout mono;
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p->setBusesLayout (mono), "mono output layout is accepted");

        p->setPlayConfigDetails (0, 1, kRate, kBlock);
        p->prepareToPlay (kRate, kBlock);
        juce::AudioBuffer<float> buffer (1, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
        process (*p, buffer, midi);
        process (*p, buffer, midi);
        check (rms (buffer, 0, kBlock) > 0.01f, "mono output carries the sound");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    testBasics();
    testSilenceThenNote();
    testSampleAccurateNoteOn();
    testNoteOffReleases();
    testParameterChangesSound();
    testStateRoundTrip();
    testProgramChange();
    testMonoOutput();

    if (failures == 0)
    {
        std::printf ("\nAll tests passed.\n");
        return 0;
    }
    std::printf ("\n%d test(s) FAILED.\n", failures);
    return 1;
}
