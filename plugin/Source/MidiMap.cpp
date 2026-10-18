#include "MidiMap.h"
#include "PresetLibrary.h" // userDataDirectory()

namespace genisys
{
namespace
{
    // Master Volume runs -24..+12 dB: this is 0 dB, so a fader at the top
    // means "as loud as normal", never +12.
    constexpr float kVolumeUnity = 24.0f / 36.0f;

    struct FactoryMapping
    {
        int cc;
        const char* paramId;
        float min = 0.0f, max = 1.0f;
    };

    // The MIDI standard's "sound controller" numbers (RP-021), which many
    // keyboards and DAW templates send: brightness on 74, attack on 73...
    const std::vector<FactoryMapping> kGeneric {
        { 74, "macro_bright" },   { 73, "macro_attack" },   { 75, "macro_decay" },
        { 72, "macro_release" },  { 71, "feedback" },       { 76, "vibrato_rate" },
        { 77, "vibrato_amount" }, { 91, "reverb_mix" },     { 93, "chorus_mix" },
        { 94, "unison_detune" },  { 5, "glide_time" },      { 7, "master_gain", 0.0f, kVolumeUnity },
    };

    // Every hardware layout gives its 8 knobs the same jobs, left to right,
    // so moving between keyboards feels the same.
    const char* const kKnobJobs[8] = { "macro_bright", "macro_attack", "macro_decay", "macro_release",
                                       "feedback", "vibrato_amount", "echo_mix", "reverb_mix" };

    std::vector<FactoryMapping> knobs (std::initializer_list<int> ccs)
    {
        std::vector<FactoryMapping> out;
        int i = 0;
        for (int cc : ccs)
            out.push_back ({ cc, kKnobJobs[i++] });
        return out;
    }

    std::vector<FactoryMapping> factoryLayout (MidiMap::Layout layout)
    {
        switch (layout)
        {
            case MidiMap::Layout::arturiaMiniLab:
            {
                // MiniLab 3 factory template: knobs 74 71 76 77 93 18 19 16,
                // faders 82 83 85 17.
                auto m = knobs ({ 74, 71, 76, 77, 93, 18, 19, 16 });
                m.push_back ({ 82, "master_gain", 0.0f, kVolumeUnity });
                m.push_back ({ 83, "psg_level" });
                m.push_back ({ 85, "drum_level" });
                m.push_back ({ 17, "glide_time" });
                return m;
            }
            case MidiMap::Layout::akaiMpkMini:
                return knobs ({ 70, 71, 72, 73, 74, 75, 76, 77 }); // MPK Mini mk3 / IV, program 1
            case MidiMap::Layout::novationLaunchkey:
                return knobs ({ 21, 22, 23, 24, 25, 26, 27, 28 }); // Launchkey pots, custom mode
            case MidiMap::Layout::generic:
            default:
                return kGeneric;
        }
    }

    const juce::Identifier mapId ("MAP"), ccId ("cc"), paramIdKey ("param"), minId ("min"), maxId ("max"),
        pickupId ("pickup"), programChangeId ("programChange");
}

const juce::Identifier MidiMap::treeType ("MIDIMAP");
const juce::String MidiMap::previousPresetId ("@preset_prev");
const juce::String MidiMap::nextPresetId ("@preset_next");

MidiMap::MidiMap (juce::AudioProcessor& processor)
{
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            params.push_back (ranged);
}

juce::String MidiMap::layoutName (Layout layout)
{
    switch (layout)
    {
        case Layout::arturiaMiniLab: return "Arturia MiniLab";
        case Layout::akaiMpkMini: return "Akai MPK Mini";
        case Layout::novationLaunchkey: return "Novation Launchkey";
        case Layout::generic:
        default: return "Generic";
    }
}

bool MidiMap::isLearnable (int cc)
{
    if (cc < 0 || cc > 119)
        return false; // 120-127 are channel-mode messages (all notes off...)
    switch (cc)
    {
        case 0: case 32:              // bank select
        case 6: case 38:              // data entry
        case 64:                      // sustain pedal
        case 98: case 99: case 100: case 101: // NRPN / RPN
            return false;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
bool MidiMap::handleController (int cc, int value)
{
    if (cc < 0 || cc > 127)
        return false;

    const int learning = learnTarget.load();
    if (learning >= 0 && isLearnable (cc))
    {
        assignIndex (cc, learning, 0.0f, 1.0f);
        learnTarget = -1;
        return true;
    }

    auto& slot = slots[(size_t) cc];
    const int index = slot.param.load();
    if (! isTarget (index))
        return false;

    if (index == previousPresetTarget || index == nextPresetTarget)
    {
        // Pads and buttons send 127 when pressed (and often 0 on release).
        if (value >= 64)
            pendingPresetSteps += index == nextPresetTarget ? 1 : -1;
        return true;
    }

    auto* param = params[(size_t) index];
    const float min = slot.min.load(), max = slot.max.load();
    // Snap stepped parameters (algorithm, switches) to a real step.
    const float target = param->convertTo0to1 (param->convertFrom0to1 (min + (max - min) * (float) value / 127.0f));
    const float current = param->getValue();

    auto& t = takeover[(size_t) cc];
    if (t.param != index) // a new mapping starts un-engaged
        t = { index, false, -1.0f, -1.0f };

    if (pickup.load())
    {
        // Something else (a preset, the mouse, automation) moved it: wait
        // for the control to catch up again.
        if (t.engaged && std::abs (current - t.lastSent) > 0.01f)
            t.engaged = false;
        if (! t.engaged)
        {
            const bool close = std::abs (target - current) <= 0.02f;
            const bool crossed = t.lastTarget >= 0.0f && (t.lastTarget - current) * (target - current) <= 0.0f;
            t.lastTarget = target;
            if (! (close || crossed))
                return true;
            t.engaged = true;
        }
    }

    t.lastTarget = target;
    t.lastSent = target;
    if (std::abs (current - target) > 1.0e-6f)
        param->setValueNotifyingHost (target);
    return true;
}

// ---------------------------------------------------------------------------
bool MidiMap::isTarget (int index) const
{
    return (index >= 0 && index < (int) params.size()) || index == previousPresetTarget || index == nextPresetTarget;
}

juce::String MidiMap::idOf (int index) const
{
    if (index == previousPresetTarget) return previousPresetId;
    if (index == nextPresetTarget) return nextPresetId;
    return index >= 0 && index < (int) params.size() ? params[(size_t) index]->getParameterID() : juce::String();
}

int MidiMap::indexOf (const juce::String& paramId) const
{
    if (paramId == previousPresetId) return previousPresetTarget;
    if (paramId == nextPresetId) return nextPresetTarget;
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i]->getParameterID() == paramId)
            return (int) i;
    return -1;
}

void MidiMap::assignIndex (int cc, int paramIndex, float min, float max)
{
    if (cc < 0 || cc > 127 || paramIndex < 0)
        return;
    // One CC per parameter: drop any older link to this parameter.
    for (auto& slot : slots)
        if (slot.param.load() == paramIndex)
            slot.param = -1;
    auto& slot = slots[(size_t) cc];
    slot.min = juce::jlimit (0.0f, 1.0f, min);
    slot.max = juce::jlimit (0.0f, 1.0f, max);
    slot.param = paramIndex;
    ++version;
}

void MidiMap::startLearning (const juce::String& paramId)
{
    learnTarget = indexOf (paramId);
    ++version;
}

void MidiMap::stopLearning()
{
    learnTarget = -1;
    ++version;
}

juce::String MidiMap::learningParamId() const
{
    return idOf (learnTarget.load());
}

void MidiMap::assign (int cc, const juce::String& paramId, float min, float max)
{
    if (isLearnable (cc))
        assignIndex (cc, indexOf (paramId), min, max);
}

void MidiMap::setRange (int cc, float min, float max)
{
    if (cc < 0 || cc > 127)
        return;
    slots[(size_t) cc].min = juce::jlimit (0.0f, 1.0f, min);
    slots[(size_t) cc].max = juce::jlimit (0.0f, 1.0f, max);
    ++version;
}

void MidiMap::forgetCc (int cc)
{
    if (cc < 0 || cc > 127)
        return;
    slots[(size_t) cc].param = -1;
    ++version;
}

void MidiMap::forgetParam (const juce::String& paramId)
{
    forgetCc (ccFor (paramId));
}

void MidiMap::clear()
{
    for (auto& slot : slots)
        slot.param = -1;
    ++version;
}

void MidiMap::loadLayout (Layout layout)
{
    clear();
    for (const auto& m : factoryLayout (layout))
        assign (m.cc, m.paramId, m.min, m.max);
}

int MidiMap::ccFor (const juce::String& paramId) const
{
    const int index = indexOf (paramId);
    if (index < 0)
        return -1;
    for (size_t cc = 0; cc < slots.size(); ++cc)
        if (slots[cc].param.load() == index)
            return (int) cc;
    return -1;
}

std::vector<MidiMap::Mapping> MidiMap::getMappings() const
{
    std::vector<Mapping> out;
    for (size_t cc = 0; cc < slots.size(); ++cc)
    {
        const int index = slots[cc].param.load();
        if (isTarget (index))
            out.push_back ({ (int) cc, idOf (index), slots[cc].min.load(), slots[cc].max.load() });
    }
    return out;
}

juce::String MidiMap::parameterName (const juce::String& paramId) const
{
    if (paramId == previousPresetId) return "Previous Preset";
    if (paramId == nextPresetId) return "Next Preset";
    if (auto* param = getParameter (paramId))
        return param->getName (64);
    return paramId;
}

juce::RangedAudioParameter* MidiMap::getParameter (const juce::String& paramId) const
{
    const int index = indexOf (paramId);
    return index >= 0 && index < (int) params.size() ? params[(size_t) index] : nullptr;
}

// ---------------------------------------------------------------------------
juce::ValueTree MidiMap::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty (pickupId, pickupEnabled(), nullptr);
    tree.setProperty (programChangeId, programChangeEnabled(), nullptr);
    for (const auto& m : getMappings())
    {
        juce::ValueTree child (mapId);
        child.setProperty (ccId, m.cc, nullptr);
        child.setProperty (paramIdKey, m.paramId, nullptr);
        child.setProperty (minId, m.min, nullptr);
        child.setProperty (maxId, m.max, nullptr);
        tree.appendChild (child, nullptr);
    }
    return tree;
}

void MidiMap::fromValueTree (const juce::ValueTree& tree)
{
    if (! tree.hasType (treeType))
        return;
    clear();
    pickup = (bool) tree.getProperty (pickupId, false);
    programChange = (bool) tree.getProperty (programChangeId, false);
    for (const auto& child : tree)
        if (child.hasType (mapId))
            assign ((int) child.getProperty (ccId, -1), child.getProperty (paramIdKey).toString(),
                    (float) child.getProperty (minId, 0.0f), (float) child.getProperty (maxId, 1.0f));
    ++version;
}

juce::File MidiMap::defaultMapFile()
{
    return userDataDirectory().getChildFile ("Default Controller Map.xml");
}

bool MidiMap::saveAsDefault() const
{
    const auto file = defaultMapFile();
    if (! file.getParentDirectory().createDirectory())
        return false;
    if (auto xml = toValueTree().createXml())
        return xml->writeTo (file);
    return false;
}

void MidiMap::loadDefault()
{
    if (auto xml = juce::XmlDocument::parse (defaultMapFile()))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.hasType (treeType))
        {
            fromValueTree (tree);
            return;
        }
    }
    loadLayout (Layout::generic);
}
}
