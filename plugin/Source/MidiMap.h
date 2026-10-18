#pragma once

#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

// MIDI Learn: links the knobs and faders on a MIDI controller (by CC
// number) to Genisys parameters.
//
// Each CC drives at most one parameter, and each parameter listens to at
// most one CC, so a mapping is easy to see and undo. A mapping can cover
// part of a parameter's range (and run backwards, with min above max).
//
// Threading: the map lives in atomics. The audio thread reads it for every
// incoming CC and moves the parameter right there, so CC moves recorded in
// a MIDI clip land at the right time even in an offline bounce. The editor
// (message thread) edits the map and starts learning.
namespace genisys
{
    class MidiMap
    {
    public:
        explicit MidiMap (juce::AudioProcessor&);

        struct Mapping
        {
            int cc = -1;
            juce::String paramId;
            float min = 0.0f, max = 1.0f; // normalised 0..1 parameter range
        };

        // Besides parameters, a CC can step through presets: pressing a
        // pad or button (any value from 64 up) moves one preset along.
        static const juce::String previousPresetId, nextPresetId;

        // Factory layouts for popular controllers (see MidiMap.cpp).
        enum class Layout { generic, arturiaMiniLab, akaiMpkMini, novationLaunchkey };
        static juce::String layoutName (Layout);

        // ---- Audio thread ----
        // Handles one incoming CC. Returns true if it was used (mapped or
        // learned), in which case nothing else should act on it.
        bool handleController (int cc, int value);

        // ---- Message thread ----
        void startLearning (const juce::String& paramId);
        void stopLearning();
        juce::String learningParamId() const;

        void assign (int cc, const juce::String& paramId, float min = 0.0f, float max = 1.0f);
        void setRange (int cc, float min, float max);
        void forgetCc (int cc);
        void forgetParam (const juce::String& paramId);
        void clear();
        void loadLayout (Layout);

        int ccFor (const juce::String& paramId) const; // -1 = not mapped
        std::vector<Mapping> getMappings() const;      // sorted by CC
        juce::String parameterName (const juce::String& paramId) const;
        juce::RangedAudioParameter* getParameter (const juce::String& paramId) const;

        // Pickup (soft takeover): a moved control only takes over once it
        // reaches the parameter's current value, so nothing jumps.
        bool pickupEnabled() const { return pickup.load(); }
        void setPickup (bool on) { pickup = on; ++version; }

        // MIDI Program Change messages pick presets (off by default).
        bool programChangeEnabled() const { return programChange.load(); }
        void setProgramChange (bool on) { programChange = on; ++version; }

        // Goes up on every change (UI refresh; telling the host the project
        // changed).
        juce::uint32 getVersion() const { return version.load(); }

        // Preset steps asked for by mapped buttons since the last call
        // (negative = back). The processor applies them on the message
        // thread, since loading a preset sets every parameter.
        bool hasPendingPresetSteps() const { return pendingPresetSteps.load() != 0; }
        int takePresetSteps() { return pendingPresetSteps.exchange (0); }

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static const juce::Identifier treeType;

        // The user's own default map, loaded by every new Genisys.
        static juce::File defaultMapFile();
        bool saveAsDefault() const;
        void loadDefault(); // the user's default if there is one, else the generic layout

        // CCs that are never learned: bank select, data entry, RPN/NRPN,
        // the sustain pedal and channel-mode messages.
        static bool isLearnable (int cc);

    private:
        int indexOf (const juce::String& paramId) const;
        bool isTarget (int index) const;
        juce::String idOf (int index) const;

        // Slot targets beyond the parameter list.
        static constexpr int previousPresetTarget = 1 << 20, nextPresetTarget = previousPresetTarget + 1;
        std::atomic<int> pendingPresetSteps { 0 };
        void assignIndex (int cc, int paramIndex, float min, float max);

        std::vector<juce::RangedAudioParameter*> params;

        struct Slot
        {
            std::atomic<int> param { -1 }; // index into params; -1 = unused
            std::atomic<float> min { 0.0f }, max { 1.0f };
        };
        std::array<Slot, 128> slots;
        std::atomic<int> learnTarget { -1 };
        std::atomic<bool> pickup { false };
        std::atomic<bool> programChange { false };
        std::atomic<juce::uint32> version { 0 };

        // Pickup state; audio thread only.
        struct Takeover
        {
            int param = -1;
            bool engaged = false;
            float lastTarget = -1.0f, lastSent = -1.0f;
        };
        std::array<Takeover, 128> takeover;
    };
}
