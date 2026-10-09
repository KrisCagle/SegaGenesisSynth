#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>

#include "genisys_patch_io.h"
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
    for (auto& channel : playedNote)
        channel.fill (-1);
    scratchLeft.resize (kScratchFrames);
    scratchRight.resize (kScratchFrames);
}

GenisysProcessor::~GenisysProcessor() = default;

void GenisysProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // A fresh engine at the host's rate. Patch and PSG settings are pushed
    // again on the next block.
    genisys_engine_init (engine.get(), sampleRate);
    forceParameterSync = true;

    editorMidi.reset (sampleRate);
    effects.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
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

    const GenisysConsoleSettings console = params.readConsole();
    if (forceParameterSync || console.chip_model != lastConsole.chip_model
        || console.filter_on != lastConsole.filter_on || console.filter_hz != lastConsole.filter_hz)
    {
        genisys_engine_set_console (engine.get(), &console);
        lastConsole = console;
    }

    const GenisysDrumSettings drums = params.readDrums();
    if (forceParameterSync || std::memcmp (&drums, &lastDrums, sizeof drums) != 0)
    {
        genisys_engine_set_drums (engine.get(), &drums);
        lastDrums = drums;
    }

    forceParameterSync = false;
}

void GenisysProcessor::handleMidi (const juce::MidiMessage& message)
{
    // General MIDI convention: channel 10 is drums. Drum hits are one-shots,
    // so their note-offs are ignored. With drums off, channel 10 plays FM.
    if (lastDrums.enabled && message.getChannel() == 10)
    {
        if (message.isNoteOn())
            genisys_engine_drum_hit (engine.get(), message.getNoteNumber(), message.getVelocity());
        return;
    }

    const auto channel = (size_t) juce::jlimit (0, 15, message.getChannel() - 1);
    const auto note = (size_t) juce::jlimit (0, 127, message.getNoteNumber());

    if (message.isPitchWheel())
    {
        // 14-bit wheel, centre 8192, mapped through the Pitch Bend Range.
        const double amount = (message.getPitchWheelValue() - 8192) / 8192.0;
        genisys_engine_pitch_bend (engine.get(), amount * params.bendRange());
        return;
    }
    if (message.isControllerOfType (1)) // mod wheel
    {
        genisys_engine_mod_wheel (engine.get(), message.getControllerValue() / 127.0);
        return;
    }
    if (message.isSustainPedalOn() || message.isSustainPedalOff())
    {
        genisys_engine_sustain (engine.get(), message.isSustainPedalOn() ? 1 : 0);
        return;
    }
    if (message.isResetAllControllers())
    {
        genisys_engine_pitch_bend (engine.get(), 0.0);
        genisys_engine_mod_wheel (engine.get(), 0.0);
        genisys_engine_sustain (engine.get(), 0);
        return;
    }

    if (message.isNoteOn())
    {
        const int played = juce::jlimit (0, 127, (int) note + 12 * params.octave());
        if (playedNote[channel][note] >= 0 && playedNote[channel][note] != played)
            genisys_engine_note_off (engine.get(), playedNote[channel][note]);
        playedNote[channel][note] = played;
        genisys_engine_note_on (engine.get(), played, message.getVelocity());
    }
    else if (message.isNoteOff())
    {
        if (playedNote[channel][note] >= 0)
            genisys_engine_note_off (engine.get(), playedNote[channel][note]);
        playedNote[channel][note] = -1;
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        genisys_engine_all_notes_off (engine.get());
        for (auto& ch : playedNote)
            ch.fill (-1);
    }
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
            const float l = scratchLeft[(size_t) i];
            const float r = scratchRight[(size_t) i];

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

    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);
    {
        juce::MidiBuffer fromEditor;
        editorMidi.removeNextBlockOfMessages (fromEditor, buffer.getNumSamples());
        midi.addEvents (fromEditor, 0, buffer.getNumSamples(), 0);
    }

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

    // Effects, then Master Volume (ramped per sample to avoid clicks).
    if (auto* host = getPlayHead())
        if (auto hostPosition = host->getPosition())
            if (auto bpm = hostPosition->getBpm())
                hostBpm = *bpm;
    effects.process (buffer, params.readEffects(), hostBpm);

    const int outputs = juce::jmin (2, buffer.getNumChannels());
    for (int i = 0; i < numSamples; ++i)
    {
        const float gain = masterGain.getNextValue();
        for (int ch = 0; ch < outputs; ++ch)
            buffer.getWritePointer (ch)[i] *= gain;
    }

    // Feed the editor's scope (mono, every 2nd sample); drop samples if it
    // isn't reading, never block.
    if (outputs > 0)
    {
        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getReadPointer (outputs > 1 ? 1 : 0);
        for (int i = 0; i < numSamples; ++i)
        {
            if (++scopeDecimate < 2)
                continue;
            scopeDecimate = 0;
            if (scopeFifo.getFreeSpace() == 0)
                break;
            const auto write = scopeFifo.write (1);
            if (write.blockSize1 > 0)
                scopeBuffer[(size_t) write.startIndex1] = 0.5f * (l[i] + r[i]);
        }
    }
    activeVoices.store (genisys_engine_active_voices (engine.get()), std::memory_order_relaxed);
}

int GenisysProcessor::readScope (float* dest, int max)
{
    const int count = juce::jmin (max, scopeFifo.getNumReady());
    const auto read = scopeFifo.read (count);
    int n = 0;
    for (int i = 0; i < read.blockSize1; ++i)
        dest[n++] = scopeBuffer[(size_t) (read.startIndex1 + i)];
    for (int i = 0; i < read.blockSize2; ++i)
        dest[n++] = scopeBuffer[(size_t) (read.startIndex2 + i)];
    return n;
}

double GenisysProcessor::getTailLengthSeconds() const
{
    return genisys::Effects::tailSeconds (params.readEffects());
}

juce::AudioProcessorEditor* GenisysProcessor::createEditor()
{
    return new GenisysEditor (*this);
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
    genisys::params::applyPreset (state, index);
}

const juce::String GenisysProcessor::getProgramName (int index)
{
    const char* name = genisys_preset_name (index);
    return name != nullptr ? juce::String (name) : juce::String();
}

juce::String GenisysProcessor::importPatchFile (const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data))
        return "Couldn't read " + file.getFileName() + ".";

    // Start from the current patch so performance settings (voice mode,
    // vibrato, velocity...) survive; the file supplies the FM sound.
    GenisysPatch patch = params.readPatch();
    const auto result = genisys_patch_import (static_cast<const uint8_t*> (data.getData()), data.getSize(),
                                              file.getFileName().toRawUTF8(), &patch);
    if (result != GENISYS_PATCH_OK)
        return genisys_patch_result_text (result);

    genisys::params::applyPatch (state, patch);
    return {};
}

juce::String GenisysProcessor::exportPatchFile (const juce::File& file)
{
    const GenisysPatch patch = params.readPatch();
    uint8_t bytes[GENISYS_TFI_SIZE];
    const auto size = genisys_patch_export_tfi (&patch, bytes);
    if (! file.replaceWithData (bytes, size))
        return "Couldn't write " + file.getFileName() + ".";
    return {};
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
