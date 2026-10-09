#pragma once

#include <memory>
#include <vector>

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
    double getTailLengthSeconds() const override { return 0.0; }

    // Factory presets, exposed through the host's program list.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState state;

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

    juce::SmoothedValue<float> masterGain; // ramps volume changes to avoid clicks
    std::vector<float> scratchLeft, scratchRight;

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenisysProcessor)
};

// Entry point JUCE's plugin wrappers (VST3, AU, Standalone) call to create
// the plugin; also used by the tests.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
