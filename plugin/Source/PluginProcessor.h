#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h> // MidiMessageCollector
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"

// The Genisys plugin: wraps one GenisysEngine and connects it to the host's
// MIDI, parameters, sample rate and saved state.
//
// Threading model (the standard JUCE one):
//  - processBlock runs on the host's audio thread and is the only place the
//    engine is touched after prepareToPlay.
//  - The UI and host automation change parameters on other threads; those
//    land in APVTS's atomics, which processBlock reads once per block.
class GenisysProcessor final : public juce::AudioProcessor
{
public:
    GenisysProcessor();
    ~GenisysProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Genisys"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    // Factory presets, exposed through the host's program list.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    // Community patch files (.tfi / .vgi / .dmp in, .tfi out). Message
    // thread only. Return an empty string on success, else the reason.
    juce::String importPatchFile (const juce::File& file);
    juce::String exportPatchFile (const juce::File& file);

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState state;

    // Notes played on the editor's on-screen keyboard (or the computer
    // keyboard), merged into the host's MIDI at the start of each block.
    juce::MidiKeyboardState keyboardState;

    // Other MIDI from the editor (on-screen pitch bend and mod wheels).
    // Thread-safe: the editor adds, processBlock drains.
    juce::MidiMessageCollector editorMidi;

private:
    void syncParametersToEngine();
    void handleMidi (const juce::MidiMessage& message);
    void render (juce::AudioBuffer<float>& buffer, int start, int count);

    // The engine holds ~100 KB of resampler tables: keep it on the heap.
    std::unique_ptr<GenisysEngine> engine;
    genisys::params::Snapshot params;

    GenisysPatch lastPatch {};
    GenisysPsgSettings lastPsg {};
    GenisysConsoleSettings lastConsole {};
    GenisysDrumSettings lastDrums {};
    bool forceParameterSync = true;

    genisys::Effects effects;
    double hostBpm = 120.0;

    juce::SmoothedValue<float> masterGain; // ramps volume changes to avoid clicks
    std::vector<float> scratchLeft, scratchRight;

    int currentProgram = 0;

    // The note each incoming (channel, note) actually started after the
    // Octave shift, so its note-off releases the same note even if the
    // octave changed while it was held. -1 = not playing.
    std::array<std::array<int, 128>, 16> playedNote;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenisysProcessor)
};

// Entry point JUCE's plugin wrappers (VST3, AU, Standalone) call to create
// the plugin; also used by the tests.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
