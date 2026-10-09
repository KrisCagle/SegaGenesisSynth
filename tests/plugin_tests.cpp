// Drives the Genisys AudioProcessor the way a DAW would (prepare, MIDI in,
// processBlock, save/restore state) without needing a DAW.

#include <cmath>
#include <functional>
#include <cstdio>
#include <memory>
#include <vector>

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
        setIntParam (*a, "chip_model", GENISYS_CHIP_YM3438);

        juce::MemoryBlock saved;
        a->getStateInformation (saved);

        auto b = makeProcessor();
        b->setStateInformation (saved.getData(), (int) saved.getSize());
        check (intParam (*b, "algorithm") == 5 && intParam (*b, "op2_tl") == 42 && intParam (*b, "velocity_sens") == 77
                   && intParam (*b, "chip_model") == GENISYS_CHIP_YM3438,
               "saved state restores every parameter (what a DAW project does on reopen)");
    }

    // What pluginval's "Plugin state restoration" test does: hosts may send
    // in-between values (e.g. from automation curves), and reloading a saved
    // state must put every parameter back exactly.
    void testStateRestoresInBetweenValues()
    {
        auto p = makeProcessor();
        juce::Random random (1234);
        auto& params = p->getParameters();

        for (auto* param : params)
            param->setValueNotifyingHost (random.nextFloat());

        std::vector<float> saved;
        for (auto* param : params)
            saved.push_back (param->getValue());

        juce::MemoryBlock state;
        p->getStateInformation (state);

        for (auto* param : params)
            param->setValueNotifyingHost (random.nextFloat());

        p->setStateInformation (state.getData(), (int) state.getSize());

        bool allRestored = true;
        for (int i = 0; i < params.size(); ++i)
        {
            if (std::abs (params[i]->getValue() - saved[(size_t) i]) > 0.001f)
            {
                std::printf ("  not restored: %s (%.3f -> %.3f)\n", params[i]->getName (64).toRawUTF8(),
                             saved[(size_t) i], params[i]->getValue());
                allRestored = false;
            }
        }
        check (allRestored, "every parameter restores exactly, even after in-between host values");
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

    void testDrumsOnChannel10()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 127), 0);
        process (*p, buffer, midi);
        process (*p, buffer, midi);
        const float drumsOffLevel = rms (buffer, 0, kBlock);

        auto q = makeProcessor();
        setIntParam (*q, "drums_on", 1);
        midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 127), 0);
        process (*q, buffer, midi);
        float peakLevel = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            process (*q, buffer, midi);
            peakLevel = juce::jmax (peakLevel, peak (buffer, 0, kBlock));
        }

        check (drumsOffLevel > 0.0f, "with drums off, channel 10 plays FM notes");
        check (peakLevel > 0.05f, "with drums on, a channel 10 kick plays the drum kit");
    }

    void testSustainPedal()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        process (*p, buffer, midi);
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        for (int i = 0; i < (int) kRate / kBlock; ++i)
            process (*p, buffer, midi);
        check (rms (buffer, 0, kBlock) > 0.005f, "sustain pedal (CC64) holds a note after key-up");

        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
        for (int i = 0; i < (int) (kRate * 3.0) / kBlock; ++i)
            process (*p, buffer, midi);
        check (peak (buffer, 0, kBlock) < 1.0e-4f, "lifting the pedal releases it");
    }

    void testOctaveShiftReleasesCorrectNote()
    {
        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        process (*p, buffer, midi);
        setIntParam (*p, "octave", 2); // change octave while the key is held
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        for (int i = 0; i < (int) (kRate * 3.0) / kBlock; ++i)
            process (*p, buffer, midi);
        check (peak (buffer, 0, kBlock) < 1.0e-4f, "a note held across an octave change still stops on key-up");
    }

    // Renders `seconds` of a 0.2 s A4 note (with a quick release) through a
    // processor, after `configure` has set its parameters.
    std::vector<float> renderNote (const std::function<void (GenisysProcessor&)>& configure, double seconds)
    {
        auto p = makeProcessor();
        for (int op = 1; op <= 4; ++op)
            setIntParam (*p, ("op" + juce::String (op) + "_rr").toRawUTF8(), 15);
        configure (*p);

        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        std::vector<float> out;
        const int blocks = (int) (kRate * seconds) / kBlock;
        for (int b = 0; b < blocks; ++b)
        {
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
            if (b == (int) (kRate * 0.2) / kBlock)
                midi.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
            process (*p, buffer, midi);
            for (int i = 0; i < kBlock; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        return out;
    }

    float rmsOf (const std::vector<float>& x, double from, double to)
    {
        double sum = 0.0;
        const auto a = (size_t) (from * kRate), b = (size_t) (to * kRate);
        for (auto i = a; i < b; ++i)
            sum += (double) x[i] * x[i];
        return (float) std::sqrt (sum / (double) (b - a));
    }

    void testEffectsOffIsUntouched()
    {
        // The same note straight from the engine, with no plugin around it.
        static GenisysEngine engine;
        genisys_engine_init (&engine, kRate);
        std::vector<float> left ((size_t) kRate), right ((size_t) kRate);
        genisys_engine_note_on (&engine, 69, 127);
        genisys_engine_render (&engine, left.data(), right.data(), (int) kRate / 2);

        auto p = makeProcessor();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
        bool identical = true;
        for (int b = 0; b < (int) (kRate / 2) / kBlock; ++b)
        {
            process (*p, buffer, midi);
            for (int i = 0; i < kBlock; ++i)
                if (buffer.getSample (0, i) != left[(size_t) (b * kBlock + i)])
                    identical = false;
        }
        check (identical, "with every effect off, the plugin's output is bit-identical to the engine's");
    }

    void testEcho()
    {
        const auto dry = renderNote ([] (GenisysProcessor&) {}, 1.0);
        const auto wet = renderNote ([] (GenisysProcessor& p)
        {
            setIntParam (p, "echo_on", 1);
            setIntParam (p, "echo_sync", 0);
            setIntParam (p, "echo_time", 250);
            setIntParam (p, "echo_feedback", 0);
            setIntParam (p, "echo_pingpong", 0);
            setIntParam (p, "echo_mix", 100);
        }, 1.0);

        std::vector<float> diff (dry.size());
        for (size_t i = 0; i < dry.size(); ++i)
            diff[i] = wet[i] - dry[i];
        check (rmsOf (diff, 0.0, 0.24) < 1.0e-6f, "echo: nothing extra before the 250 ms delay");
        check (rmsOf (diff, 0.26, 0.45) > 0.01f, "echo: a copy of the note arrives after 250 ms");
    }

    void testReverbTail()
    {
        const auto dry = renderNote ([] (GenisysProcessor&) {}, 1.5);
        const auto wet = renderNote ([] (GenisysProcessor& p)
        {
            setIntParam (p, "reverb_on", 1);
            setIntParam (p, "reverb_size", 80);
            setIntParam (p, "reverb_mix", 40);
        }, 1.5);
        check (rmsOf (dry, 1.0, 1.4) < 1.0e-4f && rmsOf (wet, 1.0, 1.4) > 1.0e-3f,
               "reverb: the sound keeps ringing after the dry note has stopped");
    }

    void testTailLength()
    {
        auto p = makeProcessor();
        const double dryTail = p->getTailLengthSeconds();
        setIntParam (*p, "reverb_on", 1);
        check (dryTail == 0.0 && p->getTailLengthSeconds() > 1.0, "the plugin reports a tail to the host only when echo/reverb are on");
    }

    void testPatchFiles()
    {
        // A TFI file built from the documented layout: algorithm 6,
        // feedback 5, OP4 (last in the file) with TL 7.
        juce::MemoryBlock tfi (42, true);
        auto* bytes = static_cast<uint8_t*> (tfi.getData());
        bytes[0] = 6;
        bytes[1] = 5;
        for (int slot = 0; slot < 4; ++slot)
        {
            bytes[2 + slot * 10 + 0] = 1;  // MUL
            bytes[2 + slot * 10 + 1] = 3;  // DT: none
            bytes[2 + slot * 10 + 2] = (uint8_t) (slot == 3 ? 7 : 40);
            bytes[2 + slot * 10 + 4] = 31; // AR
            bytes[2 + slot * 10 + 7] = 8;  // RR
        }
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("genisys_plugin_tests");
        dir.createDirectory();
        const auto in = dir.getChildFile ("test.tfi");
        in.replaceWithData (tfi.getData(), tfi.getSize());

        auto p = makeProcessor();
        setIntParam (*p, "voice_mode", GENISYS_MODE_LEGATO);
        const auto error = p->importPatchFile (in);
        check (error.isEmpty() && intParam (*p, "algorithm") == 6 && intParam (*p, "feedback") == 5
                   && intParam (*p, "op4_tl") == 7,
               "loading a .tfi file sets the FM parameters");
        check (intParam (*p, "voice_mode") == GENISYS_MODE_LEGATO, "loading a patch keeps the performance settings");

        const auto out = dir.getChildFile ("saved.tfi");
        juce::MemoryBlock saved;
        check (p->exportPatchFile (out).isEmpty() && out.loadFileAsData (saved) && saved == tfi,
               "saving writes the same TFI bytes back");
        check (p->importPatchFile (dir.getChildFile ("missing.tfi")).isNotEmpty(), "a missing file reports an error");
        dir.deleteRecursively();
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
    testStateRestoresInBetweenValues();
    testProgramChange();
    testDrumsOnChannel10();
    testSustainPedal();
    testOctaveShiftReleasesCorrectNote();
    testEffectsOffIsUntouched();
    testEcho();
    testReverbTail();
    testTailLength();
    testPatchFiles();
    testMonoOutput();

    if (failures == 0)
    {
        std::printf ("\nAll tests passed.\n");
        return 0;
    }
    std::printf ("\n%d test(s) FAILED.\n", failures);
    return 1;
}
