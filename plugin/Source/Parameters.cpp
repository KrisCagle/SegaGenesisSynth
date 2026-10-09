#include "Parameters.h"

#include "genisys_presets.h"

namespace genisys::params
{
namespace
{
    constexpr int kVersion = 1; // ParameterID version hint (needed by AU); bump only for new IDs

    using Labeller = std::function<juce::String (int)>;

    std::unique_ptr<juce::AudioParameterInt> intParam (const juce::String& id, const juce::String& name,
                                                       int lo, int hi, int def, Labeller label = {})
    {
        auto attributes = juce::AudioParameterIntAttributes();
        if (label)
            attributes = attributes.withStringFromValueFunction ([label] (int v, int) { return label (v); });
        return std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, kVersion }, name, lo, hi, def, attributes);
    }

    // An on/off switch. Built on a two-choice parameter rather than
    // juce::AudioParameterBool because AudioParameterBool keeps whatever
    // in-between value a host sends (e.g. 0.37 from an automation curve)
    // while saving it as plain "off"; reloading that state then leaves the
    // stale 0.37 in place. A choice parameter snaps every value to Off/On.
    class SwitchParameter final : public juce::AudioParameterChoice
    {
    public:
        SwitchParameter (const juce::String& id, const juce::String& name, bool def)
            : AudioParameterChoice (juce::ParameterID { id, kVersion }, name, juce::StringArray { "Off", "On" }, def ? 1 : 0)
        {
        }

        bool isBoolean() const override { return true; } // hosts show it as a toggle
    };

    std::unique_ptr<juce::AudioParameterChoice> boolParam (const juce::String& id, const juce::String& name, bool def)
    {
        return std::make_unique<SwitchParameter> (id, name, def);
    }

    // Shows register values in the units a musician thinks in, while the
    // stored value stays the exact hardware register value.
    juce::String multiplierLabel (int v) { return v == 0 ? "x0.5" : "x" + juce::String (v); }
    juce::String detuneLabel (int v)
    {
        static const char* names[] = { "0", "+1", "+2", "+3", "0", "-1", "-2", "-3" };
        return names[v & 7];
    }
    juce::String levelLabel (int v) { return v == 0 ? "0 dB" : juce::String (-0.75f * (float) v, 2) + " dB"; }
    juce::String sustainLabel (int v) { return v == 15 ? "-93 dB" : juce::String (-3 * v) + " dB"; }
    juce::String lfoRateLabel (int v)
    {
        static const char* hz[] = { "3.98 Hz", "5.56 Hz", "6.02 Hz", "6.37 Hz", "6.88 Hz", "9.63 Hz", "48.1 Hz", "72.2 Hz" };
        return hz[v & 7];
    }
    juce::String amsLabel (int v)
    {
        static const char* db[] = { "Off", "1.4 dB", "5.9 dB", "11.8 dB" };
        return db[v & 3];
    }
    juce::String pmsLabel (int v)
    {
        static const char* cents[] = { "Off", "3.4 cents", "6.7 cents", "10 cents", "14 cents", "20 cents", "40 cents", "80 cents" };
        return cents[v & 7];
    }
    juce::String percentLabel (int v) { return juce::String (v) + "%"; }
    juce::String semitoneLabel (int v) { return juce::String (v) + (v == 1 ? " semitone" : " semitones"); }
    juce::String centsLabel (int v) { return juce::String (v) + " cents"; }
    juce::String tenthsHzLabel (int v) { return juce::String (v / 10.0, 1) + " Hz"; }
    juce::String msLabel (int v) { return v == 0 ? juce::String ("Off") : juce::String (v) + " ms"; }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name,
                                                           float lo, float hi, float def, const juce::String& unit,
                                                           float step = 0.01f)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, kVersion }, name, juce::NormalisableRange<float> (lo, hi, step), def,
            juce::AudioParameterFloatAttributes().withLabel (unit));
    }

    // Echo note lengths, in quarter notes.
    const juce::StringArray kEchoDivisionNames { "1/4", "1/8", "1/8 dotted", "1/16", "1/4 dotted", "1/8 triplet" };
    constexpr float kEchoDivisionBeats[] = { 1.0f, 0.5f, 0.75f, 0.25f, 1.5f, 1.0f / 3.0f };
    juce::String unisonLabel (int v) { return v == 1 ? juce::String ("Off") : juce::String (v) + " channels"; }

    // PSG envelopes step once per 60 Hz frame (~16.7 ms).
    juce::String framesPerStepLabel (int v)
    {
        return v == 0 ? juce::String ("Instant") : juce::String (v * 1000.0 / 60.0, 0) + " ms/step";
    }
    juce::String psgVolumeLabel (int v) { return v == 0 ? juce::String ("Silent") : juce::String (-2 * (15 - v)) + " dB"; }
    juce::String octaveLabel (int v) { return v == 0 ? juce::String ("0") : (v > 0 ? "+" : "") + juce::String (v) + " oct"; }
    juce::String arpSpeedLabel (int v) { return juce::String (v * 1000.0 / 60.0, 0) + " ms"; }

    std::atomic<float>* raw (juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        auto* value = state.getRawParameterValue (id);
        jassert (value != nullptr); // an ID here doesn't match createLayout()
        return value;
    }

    int asInt (const std::atomic<float>* value) { return (int) std::lround (value->load (std::memory_order_relaxed)); }
}

juce::String opId (int op, const char* field)
{
    return "op" + juce::String (op + 1) + "_" + field;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    const GenisysPatch def = genisys_default_patch();
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (intParam ("algorithm", "Algorithm", 0, 7, def.algorithm));
    layout.add (intParam ("feedback", "Feedback", 0, 7, def.feedback));
    layout.add (boolParam ("lfo_enable", "LFO On", def.lfo_enable != 0));
    layout.add (intParam ("lfo_rate", "LFO Rate", 0, 7, def.lfo_rate, lfoRateLabel));
    layout.add (intParam ("ams", "Tremolo Depth (AMS)", 0, 3, def.ams, amsLabel));
    layout.add (intParam ("pms", "Vibrato Depth (PMS)", 0, 7, def.pms, pmsLabel));
    layout.add (intParam ("velocity_sens", "Velocity Sensitivity", 0, 100, def.velocity_sens, percentLabel));
    layout.add (intParam ("octave", "Octave", -3, 3, 0, octaveLabel));
    layout.add (intParam ("bend_range", "Pitch Bend Range", 0, 24, 2, semitoneLabel));
    layout.add (intParam ("vibrato_depth", "Mod Wheel Vibrato Depth", 0, 100, def.vibrato_depth, centsLabel));
    layout.add (intParam ("vibrato_rate", "Vibrato Speed", 10, 150, def.vibrato_rate, tenthsHzLabel));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "voice_mode", kVersion }, "Voice Mode",
        juce::StringArray { "Poly", "Mono", "Legato" }, def.voice_mode));
    layout.add (intParam ("glide_time", "Glide (Mono/Legato)", 0, 2000, def.glide_time, msLabel));
    layout.add (intParam ("unison", "Unison", 1, 3, def.unison, unisonLabel));
    layout.add (intParam ("unison_detune", "Unison Detune", 0, 50, def.unison_detune, centsLabel));
    layout.add (boolParam ("unison_stereo", "Unison Stereo", def.unison_stereo != 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "master_gain", kVersion }, "Master Volume",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    for (int op = 0; op < 4; ++op)
    {
        const auto& o = def.op[op];
        const auto name = [op] (const char* what) { return "OP" + juce::String (op + 1) + " " + what; };

        layout.add (intParam (opId (op, "mul"), name ("Multiplier (MUL)"), 0, 15, o.mul, multiplierLabel));
        layout.add (intParam (opId (op, "dt"), name ("Detune (DT)"), 0, 7, o.dt, detuneLabel));
        layout.add (intParam (opId (op, "tl"), name ("Level (TL)"), 0, 127, o.tl, levelLabel));
        layout.add (intParam (opId (op, "ar"), name ("Attack (AR)"), 0, 31, o.ar));
        layout.add (intParam (opId (op, "d1r"), name ("Decay 1 (D1R)"), 0, 31, o.d1r));
        layout.add (intParam (opId (op, "d2r"), name ("Decay 2 (D2R)"), 0, 31, o.d2r));
        layout.add (intParam (opId (op, "sl"), name ("Sustain Level (SL)"), 0, 15, o.sl, sustainLabel));
        layout.add (intParam (opId (op, "rr"), name ("Release (RR)"), 0, 15, o.rr));
        layout.add (intParam (opId (op, "ks"), name ("Key Scale (KS)"), 0, 3, o.ks));
        layout.add (boolParam (opId (op, "am"), name ("Tremolo On (AM)"), o.am != 0));
        layout.add (boolParam (opId (op, "ssg_enable"), name ("SSG-EG On"), o.ssg_enable != 0));
        layout.add (intParam (opId (op, "ssg_mode"), name ("SSG-EG Shape"), 0, 7, o.ssg_mode));
    }

    // The console's sound path after the chip (see GenisysConsoleSettings).
    const GenisysConsoleSettings console = genisys_default_console();
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "chip_model", kVersion }, "Chip",
        juce::StringArray { "YM2612 (Model 1, gritty)", "YM3438 (Model 2, cleaner)", "Clean (no DAC)" },
        console.chip_model));
    layout.add (boolParam ("console_filter", "Console Filter On", console.filter_on != 0));
    {
        juce::NormalisableRange<float> range (1000.0f, 20000.0f, 1.0f);
        range.setSkewForCentre (4000.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "filter_cutoff", kVersion }, "Console Filter Cutoff", range,
            (float) console.filter_hz,
            juce::AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (
                [] (float v, int) { return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String ((int) v) + " Hz"; })));
    }

    const GenisysPsgSettings psg = genisys_default_psg();
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "psg_mode", kVersion }, "PSG Mode",
        juce::StringArray { "Off", "Unison", "Arpeggio" }, psg.mode));
    // Effects (after the chips): Chorus -> Echo -> Reverb.
    const genisys::EffectSettings fx;
    layout.add (boolParam ("chorus_on", "Chorus On", fx.chorusOn));
    layout.add (floatParam ("chorus_rate", "Chorus Rate", 0.1f, 5.0f, fx.chorusRateHz, "Hz"));
    layout.add (intParam ("chorus_depth", "Chorus Depth", 0, 100, (int) (fx.chorusDepth * 100.0f), percentLabel));
    layout.add (intParam ("chorus_mix", "Chorus Mix", 0, 100, (int) (fx.chorusMix * 100.0f), percentLabel));
    layout.add (boolParam ("echo_on", "Echo On", fx.echoOn));
    layout.add (boolParam ("echo_sync", "Echo Sync to Tempo", fx.echoSync));
    layout.add (intParam ("echo_time", "Echo Time (unsynced)", 10, 2000, (int) fx.echoTimeMs, msLabel));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "echo_division", kVersion }, "Echo Time (synced)", kEchoDivisionNames, 2));
    layout.add (intParam ("echo_feedback", "Echo Feedback", 0, 95, (int) (fx.echoFeedback * 100.0f), percentLabel));
    layout.add (intParam ("echo_mix", "Echo Mix", 0, 100, (int) (fx.echoMix * 100.0f), percentLabel));
    layout.add (boolParam ("echo_pingpong", "Echo Ping-Pong", fx.echoPingPong));
    layout.add (boolParam ("reverb_on", "Reverb On", fx.reverbOn));
    layout.add (intParam ("reverb_size", "Reverb Size", 0, 100, (int) (fx.reverbSize * 100.0f), percentLabel));
    layout.add (intParam ("reverb_damping", "Reverb Damping", 0, 100, (int) (fx.reverbDamping * 100.0f), percentLabel));
    layout.add (intParam ("reverb_width", "Reverb Width", 0, 100, (int) (fx.reverbWidth * 100.0f), percentLabel));
    layout.add (intParam ("reverb_mix", "Reverb Mix", 0, 100, (int) (fx.reverbMix * 100.0f), percentLabel));

    layout.add (intParam ("psg_level", "PSG Level", 0, 15, psg.level, psgVolumeLabel));
    layout.add (intParam ("psg_octave", "PSG Octave", -2, 2, psg.octave, octaveLabel));
    layout.add (intParam ("psg_attack", "PSG Attack", 0, 15, psg.attack, framesPerStepLabel));
    layout.add (intParam ("psg_decay", "PSG Decay", 0, 15, psg.decay, framesPerStepLabel));
    layout.add (intParam ("psg_sustain", "PSG Sustain", 0, 15, psg.sustain, psgVolumeLabel));
    layout.add (intParam ("psg_release", "PSG Release", 0, 15, psg.release, framesPerStepLabel));
    layout.add (intParam ("psg_arp_speed", "PSG Arpeggio Speed", 1, 8, psg.arp_speed, arpSpeedLabel));

    // Drum kit on MIDI channel 10 (General MIDI drum notes).
    layout.add (boolParam ("drums_on", "Drums On (MIDI ch 10)", false));
    layout.add (intParam ("drum_level", "Drum Level", 0, 100, 80, percentLabel));
    layout.add (boolParam ("noise_on", "Noise On", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "noise_white", kVersion }, "Noise Type", juce::StringArray { "Periodic", "White" }, 1));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "noise_rate", kVersion }, "Noise Rate",
        juce::StringArray { "High", "Medium", "Low", "Follow PSG Tone 3" }, 1));
    layout.add (intParam ("noise_volume", "Noise Volume", 0, 15, psg.noise_volume));

    return layout;
}

Snapshot::Snapshot (juce::AudioProcessorValueTreeState& state)
    : algorithm (raw (state, "algorithm")), feedback (raw (state, "feedback")),
      lfoEnable (raw (state, "lfo_enable")), lfoRate (raw (state, "lfo_rate")),
      ams (raw (state, "ams")), pms (raw (state, "pms")),
      velocitySens (raw (state, "velocity_sens")), masterGain (raw (state, "master_gain")),
      octaveShift (raw (state, "octave")), bendRangeSemis (raw (state, "bend_range")),
      vibratoDepth (raw (state, "vibrato_depth")), vibratoRate (raw (state, "vibrato_rate")),
      voiceMode (raw (state, "voice_mode")), glideTime (raw (state, "glide_time")),
      unison (raw (state, "unison")), unisonDetune (raw (state, "unison_detune")),
      unisonStereo (raw (state, "unison_stereo")),
      chorusOn (raw (state, "chorus_on")), chorusRate (raw (state, "chorus_rate")),
      chorusDepth (raw (state, "chorus_depth")), chorusMix (raw (state, "chorus_mix")),
      echoOn (raw (state, "echo_on")), echoSync (raw (state, "echo_sync")), echoTime (raw (state, "echo_time")),
      echoDivision (raw (state, "echo_division")), echoFeedback (raw (state, "echo_feedback")),
      echoMix (raw (state, "echo_mix")), echoPingPong (raw (state, "echo_pingpong")),
      reverbOn (raw (state, "reverb_on")), reverbSize (raw (state, "reverb_size")),
      reverbDamping (raw (state, "reverb_damping")), reverbWidth (raw (state, "reverb_width")),
      reverbMix (raw (state, "reverb_mix")),
      psgLevel (raw (state, "psg_level")), noiseOn (raw (state, "noise_on")),
      noiseWhite (raw (state, "noise_white")), noiseRate (raw (state, "noise_rate")),
      noiseVolume (raw (state, "noise_volume")),
      chipModel (raw (state, "chip_model")), consoleFilter (raw (state, "console_filter")),
      filterCutoff (raw (state, "filter_cutoff")),
      psgMode (raw (state, "psg_mode")), psgOctave (raw (state, "psg_octave")),
      psgAttack (raw (state, "psg_attack")), psgDecay (raw (state, "psg_decay")),
      psgSustain (raw (state, "psg_sustain")), psgRelease (raw (state, "psg_release")),
      psgArpSpeed (raw (state, "psg_arp_speed")), drumsOn (raw (state, "drums_on")),
      drumLevel (raw (state, "drum_level"))
{
    for (int op = 0; op < 4; ++op)
    {
        auto& p = ops[(size_t) op];
        p.mul = raw (state, opId (op, "mul"));
        p.dt = raw (state, opId (op, "dt"));
        p.tl = raw (state, opId (op, "tl"));
        p.ar = raw (state, opId (op, "ar"));
        p.d1r = raw (state, opId (op, "d1r"));
        p.d2r = raw (state, opId (op, "d2r"));
        p.sl = raw (state, opId (op, "sl"));
        p.rr = raw (state, opId (op, "rr"));
        p.ks = raw (state, opId (op, "ks"));
        p.am = raw (state, opId (op, "am"));
        p.ssgEnable = raw (state, opId (op, "ssg_enable"));
        p.ssgMode = raw (state, opId (op, "ssg_mode"));
    }
}

GenisysPatch Snapshot::readPatch() const
{
    GenisysPatch patch {};
    patch.algorithm = asInt (algorithm);
    patch.feedback = asInt (feedback);
    patch.lfo_enable = asInt (lfoEnable);
    patch.lfo_rate = asInt (lfoRate);
    patch.ams = asInt (ams);
    patch.pms = asInt (pms);
    patch.velocity_sens = asInt (velocitySens);
    patch.vibrato_depth = asInt (vibratoDepth);
    patch.vibrato_rate = asInt (vibratoRate);
    patch.voice_mode = asInt (voiceMode);
    patch.glide_time = asInt (glideTime);
    patch.unison = asInt (unison);
    patch.unison_detune = asInt (unisonDetune);
    patch.unison_stereo = asInt (unisonStereo);

    for (size_t op = 0; op < 4; ++op)
    {
        const auto& p = ops[op];
        auto& o = patch.op[op];
        o.mul = asInt (p.mul); o.dt = asInt (p.dt); o.tl = asInt (p.tl);
        o.ar = asInt (p.ar); o.d1r = asInt (p.d1r); o.d2r = asInt (p.d2r);
        o.sl = asInt (p.sl); o.rr = asInt (p.rr); o.ks = asInt (p.ks);
        o.am = asInt (p.am); o.ssg_enable = asInt (p.ssgEnable); o.ssg_mode = asInt (p.ssgMode);
    }
    return patch;
}

GenisysPsgSettings Snapshot::readPsg() const
{
    GenisysPsgSettings psg {};
    psg.mode = asInt (psgMode);
    psg.level = asInt (psgLevel);
    psg.octave = asInt (psgOctave);
    psg.attack = asInt (psgAttack);
    psg.decay = asInt (psgDecay);
    psg.sustain = asInt (psgSustain);
    psg.release = asInt (psgRelease);
    psg.arp_speed = asInt (psgArpSpeed);
    psg.noise_on = asInt (noiseOn);
    psg.noise_white = asInt (noiseWhite);
    psg.noise_rate = asInt (noiseRate);
    psg.noise_volume = asInt (noiseVolume);
    return psg;
}

EffectSettings Snapshot::readEffects() const
{
    const auto percent = [] (const std::atomic<float>* v) { return (float) asInt (v) / 100.0f; };
    EffectSettings fx;
    fx.chorusOn = asInt (chorusOn) != 0;
    fx.chorusRateHz = chorusRate->load (std::memory_order_relaxed);
    fx.chorusDepth = percent (chorusDepth);
    fx.chorusMix = percent (chorusMix);
    fx.echoOn = asInt (echoOn) != 0;
    fx.echoSync = asInt (echoSync) != 0;
    fx.echoTimeMs = (float) asInt (echoTime);
    fx.echoBeats = kEchoDivisionBeats[juce::jlimit (0, 5, asInt (echoDivision))];
    fx.echoFeedback = percent (echoFeedback);
    fx.echoMix = percent (echoMix);
    fx.echoPingPong = asInt (echoPingPong) != 0;
    fx.reverbOn = asInt (reverbOn) != 0;
    fx.reverbSize = percent (reverbSize);
    fx.reverbDamping = percent (reverbDamping);
    fx.reverbWidth = percent (reverbWidth);
    fx.reverbMix = percent (reverbMix);
    return fx;
}

GenisysDrumSettings Snapshot::readDrums() const
{
    GenisysDrumSettings drums {};
    drums.enabled = asInt (drumsOn);
    drums.level = asInt (drumLevel);
    return drums;
}

GenisysConsoleSettings Snapshot::readConsole() const
{
    GenisysConsoleSettings console {};
    console.chip_model = asInt (chipModel);
    console.filter_on = asInt (consoleFilter);
    console.filter_hz = (double) filterCutoff->load (std::memory_order_relaxed);
    return console;
}

void applyPreset (juce::AudioProcessorValueTreeState& state, int index)
{
    const auto set = [&state] (const juce::String& id, float value)
    {
        if (auto* param = state.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };

    applyPatch (state, genisys_preset_patch (index));

    const GenisysPsgSettings psg = genisys_preset_psg (index);
    set ("psg_mode", (float) psg.mode);
    set ("psg_level", (float) psg.level);
    set ("psg_octave", (float) psg.octave);
    set ("psg_attack", (float) psg.attack);
    set ("psg_decay", (float) psg.decay);
    set ("psg_sustain", (float) psg.sustain);
    set ("psg_release", (float) psg.release);
    set ("psg_arp_speed", (float) psg.arp_speed);
    set ("noise_on", (float) psg.noise_on);

    const GenisysPresetFx fx = genisys_preset_fx (index);
    const EffectSettings defaults;
    set ("chorus_on", fx.chorus_mix > 0 ? 1.0f : 0.0f);
    set ("chorus_mix", (float) (fx.chorus_mix > 0 ? fx.chorus_mix : (int) (defaults.chorusMix * 100.0f)));
    set ("chorus_rate", defaults.chorusRateHz);
    set ("chorus_depth", defaults.chorusDepth * 100.0f);
    set ("echo_on", fx.echo_mix > 0 ? 1.0f : 0.0f);
    set ("echo_sync", 1.0f);
    set ("echo_mix", (float) (fx.echo_mix > 0 ? fx.echo_mix : (int) (defaults.echoMix * 100.0f)));
    set ("echo_feedback", (float) fx.echo_feedback);
    set ("echo_division", (float) fx.echo_division);
    set ("echo_pingpong", 1.0f);
    set ("reverb_on", fx.reverb_mix > 0 ? 1.0f : 0.0f);
    set ("reverb_mix", (float) (fx.reverb_mix > 0 ? fx.reverb_mix : (int) (defaults.reverbMix * 100.0f)));
    set ("reverb_size", (float) fx.reverb_size);
}

void applyPatch (juce::AudioProcessorValueTreeState& state, const GenisysPatch& patch)
{
    const auto set = [&state] (const juce::String& id, int value)
    {
        if (auto* param = state.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 ((float) value));
    };

    set ("algorithm", patch.algorithm);
    set ("feedback", patch.feedback);
    set ("lfo_enable", patch.lfo_enable);
    set ("lfo_rate", patch.lfo_rate);
    set ("ams", patch.ams);
    set ("pms", patch.pms);
    set ("velocity_sens", patch.velocity_sens);
    set ("vibrato_depth", patch.vibrato_depth);
    set ("vibrato_rate", patch.vibrato_rate);
    set ("voice_mode", patch.voice_mode);
    set ("glide_time", patch.glide_time);
    set ("unison", patch.unison);
    set ("unison_detune", patch.unison_detune);
    set ("unison_stereo", patch.unison_stereo);

    for (int op = 0; op < 4; ++op)
    {
        const auto& o = patch.op[op];
        set (opId (op, "mul"), o.mul); set (opId (op, "dt"), o.dt); set (opId (op, "tl"), o.tl);
        set (opId (op, "ar"), o.ar); set (opId (op, "d1r"), o.d1r); set (opId (op, "d2r"), o.d2r);
        set (opId (op, "sl"), o.sl); set (opId (op, "rr"), o.rr); set (opId (op, "ks"), o.ks);
        set (opId (op, "am"), o.am); set (opId (op, "ssg_enable"), o.ssg_enable); set (opId (op, "ssg_mode"), o.ssg_mode);
    }
}
}
