#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Effects.h"
#include "genisys_engine.h" // C header with its own extern "C" guards

// Every user-facing control is a host parameter, so it can be automated,
// saved with the DAW project, and recalled exactly.
//
// Parameter IDs are a public contract: DAW projects and automation lanes
// store them. Never rename or reuse an ID once released; add new ones
// instead (and bump the version number passed to juce::ParameterID).
namespace genisys::params
{
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::String opId (int op, const char* field); // op is 0-3 -> "op1_<field>" .. "op4_<field>"

    // Lock-free view of the parameters for the audio thread: pointers to the
    // atomic values APVTS keeps, looked up once at construction.
    class Snapshot
    {
    public:
        explicit Snapshot (juce::AudioProcessorValueTreeState& state);

        GenisysPatch readPatch() const;
        GenisysPsgSettings readPsg() const;
        GenisysConsoleSettings readConsole() const;
        GenisysDrumSettings readDrums() const;
        EffectSettings readEffects() const;
        float masterGainDb() const { return masterGain->load(); }
        int octave() const { return (int) std::lround (octaveShift->load (std::memory_order_relaxed)); }
        int bendRange() const { return (int) std::lround (bendRangeSemis->load (std::memory_order_relaxed)); }

    private:
        struct OpParams
        {
            std::atomic<float>* mul; std::atomic<float>* dt; std::atomic<float>* tl;
            std::atomic<float>* ar; std::atomic<float>* d1r; std::atomic<float>* d2r;
            std::atomic<float>* sl; std::atomic<float>* rr; std::atomic<float>* ks;
            std::atomic<float>* am; std::atomic<float>* ssgEnable; std::atomic<float>* ssgMode;
        };

        std::atomic<float>* algorithm; std::atomic<float>* feedback;
        std::atomic<float>* lfoEnable; std::atomic<float>* lfoRate;
        std::atomic<float>* ams; std::atomic<float>* pms;
        std::atomic<float>* velocitySens; std::atomic<float>* masterGain; std::atomic<float>* octaveShift;
        std::atomic<float>* bendRangeSemis; std::atomic<float>* vibratoDepth; std::atomic<float>* vibratoRate;
        std::atomic<float>* voiceMode; std::atomic<float>* glideTime; std::atomic<float>* unison;
        std::atomic<float>* unisonDetune; std::atomic<float>* unisonStereo;
        std::atomic<float>* macroBright; std::atomic<float>* macroAttack; std::atomic<float>* macroDecay;
        std::atomic<float>* macroRelease; std::atomic<float>* vibratoAmount;
        std::atomic<float>* chorusOn; std::atomic<float>* chorusRate; std::atomic<float>* chorusDepth;
        std::atomic<float>* chorusMix; std::atomic<float>* echoOn; std::atomic<float>* echoSync;
        std::atomic<float>* echoTime; std::atomic<float>* echoDivision; std::atomic<float>* echoFeedback;
        std::atomic<float>* echoMix; std::atomic<float>* echoPingPong; std::atomic<float>* reverbOn;
        std::atomic<float>* reverbSize; std::atomic<float>* reverbDamping; std::atomic<float>* reverbWidth;
        std::atomic<float>* reverbMix;
        std::atomic<float>* psgLevel; std::atomic<float>* noiseOn; std::atomic<float>* noiseWhite;
        std::atomic<float>* noiseRate; std::atomic<float>* noiseVolume;
        std::atomic<float>* chipModel; std::atomic<float>* consoleFilter; std::atomic<float>* filterCutoff;
        std::atomic<float>* psgMode; std::atomic<float>* psgOctave; std::atomic<float>* psgAttack;
        std::atomic<float>* psgDecay; std::atomic<float>* psgSustain; std::atomic<float>* psgRelease;
        std::atomic<float>* psgArpSpeed; std::atomic<float>* drumsOn; std::atomic<float>* drumLevel;
        std::array<OpParams, 4> ops;
    };

    // Sets every patch parameter from a GenisysPatch, notifying the host
    // (used when loading a factory preset). Message thread only.
    void applyPatch (juce::AudioProcessorValueTreeState& state, const GenisysPatch& patch);

    // Loads factory preset `index`: its sound and performance settings, PSG
    // layer and effects. Global settings (Master Volume, Octave, bend range,
    // chip model, console filter, drums) are left as they are. Message thread.
    void applyPreset (juce::AudioProcessorValueTreeState& state, int index);
}
