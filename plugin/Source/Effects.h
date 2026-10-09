#pragma once

#include <juce_dsp/juce_dsp.h>

// The effects chain after the chips: Chorus -> Echo -> Reverb.
//
// The Genesis had no effects hardware (games faked echo by replaying parts
// on spare channels), so this is the plugin's modern addition. Each effect is
// skipped entirely when off, so with everything off the chip sound passes
// through untouched.
namespace genisys
{
    struct EffectSettings
    {
        bool chorusOn = false;
        float chorusRateHz = 0.8f, chorusDepth = 0.35f, chorusMix = 0.5f;

        bool echoOn = false;
        float echoTimeMs = 350.0f;      // used when not synced
        bool echoSync = true;
        float echoBeats = 0.75f;        // synced length, in quarter notes
        float echoFeedback = 0.35f, echoMix = 0.3f;
        bool echoPingPong = true;

        bool reverbOn = false;
        float reverbSize = 0.5f, reverbDamping = 0.5f, reverbWidth = 1.0f, reverbMix = 0.25f;
    };

    class Effects
    {
    public:
        void prepare (double sampleRate, int maximumBlockSize, int numChannels);
        void reset();

        // `bpm` is the host tempo, for a synced echo.
        void process (juce::AudioBuffer<float>& buffer, const EffectSettings& settings, double bpm);

        // How long sound can keep ringing after input stops (for hosts).
        static double tailSeconds (const EffectSettings& settings);

    private:
        void processEcho (juce::AudioBuffer<float>& buffer, const EffectSettings& settings, double bpm);

        double sampleRate = 44100.0;
        int channels = 2;

        juce::dsp::Chorus<float> chorus;

        static constexpr double kMaxEchoSeconds = 2.0;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> echoLine;
        juce::SmoothedValue<float> echoDelaySamples;
        float echoDamping[2] {};

        juce::Reverb reverb;

        // Each effect starts clean when switched on (no stale tail bursts out).
        bool chorusWasOn = false, echoWasOn = false, reverbWasOn = false;
    };
}
