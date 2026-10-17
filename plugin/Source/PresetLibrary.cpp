#include "PresetLibrary.h"

#include <map>

namespace genisys
{
namespace
{
    const juce::Identifier presetTag ("GENISYSPRESET"), paramTag ("PARAM"), favoritesTag ("FAVORITES"), itemTag ("ITEM");

    // File names can't contain these on some systems.
    juce::String safeFileName (const juce::String& s)
    {
        auto clean = juce::File::createLegalFileName (s.trim());
        return clean.isEmpty() ? juce::String ("Untitled") : clean;
    }
}

juce::File userDataDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    dir = dir.getChildFile ("Application Support"); // ~/Library/Application Support
#endif
    return dir.getChildFile ("Genisys");
}

PresetLibrary::PresetLibrary (juce::File r) : root (std::move (r)) {}

bool PresetLibrary::isGlobalSetting (const juce::String& id)
{
    static const juce::StringArray globals { "master_gain", "octave", "bend_range", "chip_model", "console_filter",
                                             "filter_cutoff", "drums_on", "drum_level", "drum_input" };
    return globals.contains (id);
}

std::vector<PresetLibrary::UserPreset> PresetLibrary::scan() const
{
    std::vector<UserPreset> out;
    const auto dir = presetsDirectory();
    if (! dir.isDirectory())
        return out;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, true, juce::String ("*") + fileExtension))
    {
        const auto file = entry.getFile();
        const auto parent = file.getParentDirectory();
        out.push_back ({ file.getFileNameWithoutExtension(), parent == dir ? juce::String ("User") : parent.getFileName(), file });
    }
    std::sort (out.begin(), out.end(), [] (const UserPreset& a, const UserPreset& b)
    {
        const int c = a.category.compareIgnoreCase (b.category);
        return c != 0 ? c < 0 : a.name.compareIgnoreCase (b.name) < 0;
    });
    return out;
}

juce::File PresetLibrary::save (const juce::AudioProcessorValueTreeState& state, const juce::String& name,
                                const juce::String& category) const
{
    const auto dir = presetsDirectory().getChildFile (safeFileName (category.isEmpty() ? juce::String ("User") : category));
    if (! dir.createDirectory())
        return {};
    const auto file = dir.getChildFile (safeFileName (name) + fileExtension);

    juce::XmlElement xml (presetTag);
    xml.setAttribute ("name", name.trim());
    xml.setAttribute ("category", category.trim());
    xml.setAttribute ("version", 1);
    for (auto* p : state.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr || isGlobalSetting (ranged->getParameterID()))
            continue;
        auto* child = xml.createNewChildElement (paramTag);
        child->setAttribute ("id", ranged->getParameterID());
        // Saved in the parameter's own units, so files stay readable.
        child->setAttribute ("value", ranged->convertFrom0to1 (ranged->getValue()));
    }
    return xml.writeTo (file) ? file : juce::File();
}

PresetLibrary::UserPreset PresetLibrary::load (juce::AudioProcessorValueTreeState& state, const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (presetTag))
        return {};

    // Settings the file doesn't mention (added in a later version) go to
    // their defaults, so the sound is the same every time it loads.
    std::map<juce::String, float> values;
    for (auto* child : xml->getChildWithTagNameIterator (paramTag))
        values[child->getStringAttribute ("id")] = (float) child->getDoubleAttribute ("value");

    for (auto* p : state.processor.getParameters())
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (param == nullptr || isGlobalSetting (param->getParameterID()))
            continue;
        const auto found = values.find (param->getParameterID());
        const float normalised = found != values.end() ? param->convertTo0to1 (found->second) : param->getDefaultValue();
        if (param->getValue() != normalised)
            param->setValueNotifyingHost (normalised);
    }
    auto name = xml->getStringAttribute ("name");
    if (name.isEmpty())
        name = file.getFileNameWithoutExtension();
    return { name, xml->getStringAttribute ("category", "User"), file };
}

juce::String PresetLibrary::userKey (const juce::File& presetFile) const
{
    return "user:" + presetFile.getRelativePathFrom (presetsDirectory()).replaceCharacter ('\\', '/');
}

juce::File PresetLibrary::fileForUserKey (const juce::String& key) const
{
    return key.startsWith ("user:") ? presetsDirectory().getChildFile (key.fromFirstOccurrenceOf ("user:", false, false)) : juce::File();
}

juce::StringArray PresetLibrary::favorites() const
{
    juce::StringArray out;
    if (auto xml = juce::XmlDocument::parse (root.getChildFile ("Favorites.xml")))
        for (auto* item : xml->getChildWithTagNameIterator (itemTag))
            out.addIfNotAlreadyThere (item->getStringAttribute ("key"));
    return out;
}

void PresetLibrary::setFavorite (const juce::String& key, bool favorite) const
{
    auto list = favorites();
    if (favorite)
        list.addIfNotAlreadyThere (key);
    else
        list.removeString (key);

    juce::XmlElement xml (favoritesTag);
    for (const auto& k : list)
        xml.createNewChildElement (itemTag)->setAttribute ("key", k);
    if (root.createDirectory())
        xml.writeTo (root.getChildFile ("Favorites.xml"));
}
}
