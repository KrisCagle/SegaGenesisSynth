#include "Effects.h"

namespace genisys
{
void Effects::prepare (double newSampleRate, int maximumBlockSize, int numChannels)
{
    sampleRate = newSampleRate;
    channels = juce::jlimit (1, 2, numChannels);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maximumBlockSize, (juce::uint32) channels };
    chorus.prepare (spec);
    chorus.setCentreDelay (7.0f);
    chorus.setFeedback (0.0f);

    echoLine.prepare ({ sampleRate, (juce::uint32) maximumBlockSize, 2 });
    echoLine.setMaximumDelayInSamples ((int) (kMaxEchoSeconds * sampleRate) + 2);
    echoDelaySamples.reset (sampleRate, 0.05); // glide time changes instead of clicking

    reverb.setSampleRate (sampleRate);
    reset();
}

void Effects::reset()
{
    chorus.reset();
    echoLine.reset();
    echoDamping[0] = echoDamping[1] = 0.0f;
    reverb.reset();
    chorusWasOn = echoWasOn = reverbWasOn = false;
}

double Effects::tailSeconds (const EffectSettings& s)
{
    double tail = 0.0;
    if (s.echoOn)
        tail += kMaxEchoSeconds * 4.0; // repeats die away over a few delay lengths
    if (s.reverbOn)
        tail += 1.0 + 4.0 * s.reverbSize;
    return tail;
}

void Effects::processEcho (juce::AudioBuffer<float>& buffer, const EffectSettings& s, double bpm)
{
    const double seconds = s.echoSync ? s.echoBeats * 60.0 / juce::jlimit (20.0, 400.0, bpm)
                                      : s.echoTimeMs / 1000.0;
    const auto target = (float) juce::jlimit (1.0, kMaxEchoSeconds * sampleRate, seconds * sampleRate);
    if (! echoWasOn)
    {
        echoLine.reset();
        echoDamping[0] = echoDamping[1] = 0.0f;
        echoDelaySamples.setCurrentAndTargetValue (target);
    }
    echoDelaySamples.setTargetValue (target);

    const float feedback = juce::jlimit (0.0f, 0.95f, s.echoFeedback);
    const bool pingPong = s.echoPingPong && buffer.getNumChannels() >= 2;
    constexpr float damping = 0.35f; // each repeat a little darker, like tape (or a quieter game channel)

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() >= 2 ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const float delay = echoDelaySamples.getNextValue();
        echoLine.setDelay (delay);

        const float inL = left[i];
        const float inR = right != nullptr ? right[i] : inL;
        float dl = echoLine.popSample (0);
        float dr = echoLine.popSample (1);
        echoDamping[0] += damping * (dl - echoDamping[0]);
        echoDamping[1] += damping * (dr - echoDamping[1]);
        dl = echoDamping[0];
        dr = echoDamping[1];

        if (pingPong)
        {
            // The input enters on the left; each repeat crosses to the other side.
            echoLine.pushSample (0, 0.5f * (inL + inR) + feedback * dr);
            echoLine.pushSample (1, feedback * dl);
        }
        else
        {
            echoLine.pushSample (0, inL + feedback * dl);
            echoLine.pushSample (1, inR + feedback * dr);
        }

        left[i] = inL + s.echoMix * dl;
        if (right != nullptr)
            right[i] = inR + s.echoMix * dr;
    }
}

void Effects::process (juce::AudioBuffer<float>& buffer, const EffectSettings& s, double bpm)
{
    if (s.chorusOn && ! chorusWasOn)
        chorus.reset();
    if (s.reverbOn && ! reverbWasOn)
        reverb.reset();

    if (s.chorusOn)
    {
        chorus.setRate (s.chorusRateHz);
        chorus.setDepth (s.chorusDepth);
        chorus.setMix (s.chorusMix);
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) juce::jmin (channels, buffer.getNumChannels()),
                                            (size_t) buffer.getNumSamples());
        chorus.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    if (s.echoOn)
        processEcho (buffer, s, bpm);

    if (s.reverbOn)
    {
        juce::Reverb::Parameters p;
        p.roomSize = s.reverbSize;
        p.damping = s.reverbDamping;
        p.width = s.reverbWidth;
        p.wetLevel = s.reverbMix;
        p.dryLevel = 1.0f - 0.5f * s.reverbMix;
        p.freezeMode = 0.0f;
        reverb.setParameters (p);

        if (buffer.getNumChannels() >= 2)
            reverb.processStereo (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
        else
            reverb.processMono (buffer.getWritePointer (0), buffer.getNumSamples());
    }

    chorusWasOn = s.chorusOn;
    echoWasOn = s.echoOn;
    reverbWasOn = s.reverbOn;
}
}
