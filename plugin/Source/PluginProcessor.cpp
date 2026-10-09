#include "PluginProcessor.h"

#include <cstring>

#include "genisys_presets.h"

namespace
{
    // Builds the chip lookup tables exactly once per process. C++ guarantees
    // a function-local static is initialised once even if several plugin
    // instances are created on different threads at the same moment.
    void ensureEngineGlobalsInitialised()
    {
        static const bool initialised = [] { genisys_engine_global_init(); return true; }();
        juce::ignoreUnused (initialised);
    }

    constexpr int kScratchFrames = 1024;
}

GenisysProcessor::GenisysProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "GenisysState", genisys::params::createLayout()),
      engine ((ensureEngineGlobalsInitialised(), std::make_unique<GenisysEngine>())),
      params (state)
{
    genisys_engine_init (engine.get(), 44100.0);
    scratchLeft.resize (kScratchFrames);
    scratchRight.resize (kScratchFrames);
}

GenisysProcessor::~GenisysProcessor() = default;

void GenisysProcessor::prepareToPlay (double sampleRate, int)
{
    // A fresh engine at the host's rate. Patch and PSG settings are pushed
    // again on the next block.
    genisys_engine_init (engine.get(), sampleRate);
    forceParameterSync = true;

    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (params.masterGainDb()));
}

bool GenisysProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void GenisysProcessor::syncParametersToEngine()
{
    // Only touch the chip when something actually changed: a patch write is
    // ~170 register writes across the 6 channels.
    const GenisysPatch patch = params.readPatch();
    if (forceParameterSync || std::memcmp (&patch, &lastPatch, sizeof patch) != 0)
    {
        genisys_engine_set_patch (engine.get(), &patch);
        lastPatch = patch;
    }

    const GenisysPsgSettings psg = params.readPsg();
    if (forceParameterSync || std::memcmp (&psg, &lastPsg, sizeof psg) != 0)
    {
        genisys_engine_set_psg (engine.get(), &psg);
        lastPsg = psg;
    }

    forceParameterSync = false;
}

void GenisysProcessor::handleMidi (const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        genisys_engine_note_on (engine.get(), message.getNoteNumber(), message.getVelocity());
    else if (message.isNoteOff())
        genisys_engine_note_off (engine.get(), message.getNoteNumber());
    else if (message.isAllNotesOff() || message.isAllSoundOff())
        genisys_engine_all_notes_off (engine.get());
}

void GenisysProcessor::render (juce::AudioBuffer<float>& buffer, int start, int count)
{
    const int channels = buffer.getNumChannels();

    while (count > 0)
    {
        const int n = juce::jmin (count, kScratchFrames);
        genisys_engine_render (engine.get(), scratchLeft.data(), scratchRight.data(), n);

        for (int i = 0; i < n; ++i)
        {
            const float gain = masterGain.getNextValue();
            const float l = scratchLeft[(size_t) i] * gain;
            const float r = scratchRight[(size_t) i] * gain;

            if (channels >= 2)
            {
                buffer.setSample (0, start + i, l);
                buffer.setSample (1, start + i, r);
            }
            else if (channels == 1)
            {
                buffer.setSample (0, start + i, 0.5f * (l + r));
            }
        }

        start += n;
        count -= n;
    }
}

void GenisysProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    syncParametersToEngine();
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (params.masterGainDb()));

    // Render up to each MIDI event's exact sample position, so note timing
    // is sample-accurate no matter how large the host's buffer is.
    const int numSamples = buffer.getNumSamples();
    int position = 0;

    for (const auto metadata : midi)
    {
        const int eventPosition = juce::jlimit (position, numSamples, metadata.samplePosition);
        render (buffer, position, eventPosition - position);
        handleMidi (metadata.getMessage());
        position = eventPosition;
    }

    render (buffer, position, numSamples - position);
}

juce::AudioProcessorEditor* GenisysProcessor::createEditor()
{
    // Temporary: the host-style list of every parameter. The themed editor
    // replaces this in Phase 5 of the roadmap.
    return new juce::GenericAudioProcessorEditor (*this);
}

int GenisysProcessor::getNumPrograms()
{
    return genisys_preset_count();
}

void GenisysProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= genisys_preset_count())
        return;

    currentProgram = index;
    genisys::params::applyPatch (state, genisys_preset_patch (index));
}

const juce::String GenisysProcessor::getProgramName (int index)
{
    const char* name = genisys_preset_name (index);
    return name != nullptr ? juce::String (name) : juce::String();
}

void GenisysProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto copy = state.copyState();
    copy.setProperty ("program", currentProgram, nullptr);
    if (auto xml = copy.createXml())
        copyXmlToBinary (*xml, destData);
}

void GenisysProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (state.state.getType()))
        {
            auto restored = juce::ValueTree::fromXml (*xml);
            currentProgram = restored.getProperty ("program", 0);
            state.replaceState (restored);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GenisysProcessor();
}
