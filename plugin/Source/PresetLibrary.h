#pragma once

#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

// The user's own presets and favourites, kept as files in Genisys's folder:
//
//   <user data>/Genisys/Presets/<Category>/<Name>.genisyspreset
//   <user data>/Genisys/Favorites.xml
//
// (<user data> is ~/Library/Application Support on a Mac, %APPDATA% on
// Windows, ~/.config on Linux.) A preset file is plain XML listing
// parameter values, so friends can swap them. Message thread only.
namespace genisys
{
    // Genisys's folder for user files (created when first written).
    juce::File userDataDirectory();

    class PresetLibrary
    {
    public:
        // `root` is the Genisys folder; tests pass a temporary one.
        explicit PresetLibrary (juce::File root = userDataDirectory());

        struct UserPreset
        {
            juce::String name, category;
            juce::File file;
        };

        static constexpr const char* fileExtension = ".genisyspreset";

        juce::File presetsDirectory() const { return root.getChildFile ("Presets"); }
        std::vector<UserPreset> scan() const; // sorted by category, then name

        // Saves every sound setting (FM, PSG, effects, play mode). Settings
        // that belong to the player rather than the sound (Master Volume,
        // Octave, bend range, chip, console filter, drums) are left out, as
        // for factory presets. Returns the file, or an empty File on error.
        juce::File save (const juce::AudioProcessorValueTreeState&, const juce::String& name, const juce::String& category) const;

        // Applies a preset file. Returns its name and category, or an
        // empty name if the file isn't a Genisys preset.
        static UserPreset load (juce::AudioProcessorValueTreeState&, const juce::File&);

        static bool isGlobalSetting (const juce::String& paramId);

        // Favourites are keys: "factory:<name>" or "user:<path inside Presets>".
        static juce::String factoryKey (const juce::String& name) { return "factory:" + name; }
        juce::String userKey (const juce::File& presetFile) const;
        juce::File fileForUserKey (const juce::String& key) const;
        juce::StringArray favorites() const;
        bool isFavorite (const juce::String& key) const { return favorites().contains (key); }
        void setFavorite (const juce::String& key, bool favorite) const;

    private:
        juce::File root;
    };
}
