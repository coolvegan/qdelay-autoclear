// Copyright 2025 tilr
// Auto Clear - smooth, note-triggered delay tail termination (Air Delay Pro style)
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

/**
    Auto Clear watches the dry input (and optionally incoming MIDI) for a new note
    and ducks the wet signal, holds, then releases over the Clear Fade time. At
    100% the delay line is wiped once the wet path is effectively silent.

    The gain is a continuous function of the detector, not a gate: there is no
    active/hold/fade state machine, so the end of a phrase releases
    monotonically instead of fluttering between duck and open.
*/
class AutoClear
{
public:
    AutoClear() = default;

    void prepare (double sampleRate)
    {
        srate = (float) std::max (8000.0, sampleRate);
        detAttack  = 1.0f - std::exp (-1.0f / (0.005f * srate));
        detRelease = 1.0f - std::exp (-1.0f / (0.300f * srate));
        duckAttack = 1.0f - std::exp (-1.0f / (0.005f * srate));
        midiDecay  = 1.0f - std::exp (-1.0f / (0.300f * srate));
        reset();
    }

    void reset()
    {
        env = 0.0f;
        gain = 1.0f;
        holdCounter = 0;
        midiEnv = 0.0f;
        resetDone = false;
    }

    void setParameters (bool enabled_,
                        float fadeMs_,
                        float thresholdDb_,
                        float holdMs_,
                        float amount_,
                        int source_)
    {
        enabled = enabled_;
        fadeMs = juce::jlimit (2.0f, 1000.0f, fadeMs_);
        holdSamples = std::max (0, (int) (juce::jlimit (0.0f, 2000.0f, holdMs_) * 0.001f * srate));
        amount = juce::jlimit (0.0f, 1.0f, amount_);
        threshold = std::pow (10.0f, juce::jlimit (-80.0f, 0.0f, thresholdDb_) / 20.0f);
        source = juce::jlimit (0, 2, source_); // 0 audio, 1 midi, 2 both

        // The release is the user's Clear Fade.
        release = 1.0f - std::exp (-1.0f / (fadeMs * 0.001f * srate));

        if (! enabled || amount <= 0.0001f)
        {
            env = 0.0f;
            gain = 1.0f;
            holdCounter = 0;
            midiEnv = 0.0f;
            resetDone = false;
        }
    }

    /** Call once at the start of each block. */
    void beginBlock (bool midiNoteOn)
    {
        if (enabled && amount > 0.0001f && midiNoteOn && (source == 1 || source == 2))
            midiEnv = 1.0f;
    }

    /**
        Per-sample update.

        @returns the wet gain for this sample. When @p clearNow is set the caller
                 must clear the delay line before the next sample is processed.
    */
    float process (float left, float right, bool& clearNow)
    {
        clearNow = false;

        if (! enabled || amount <= 0.0001f)
        {
            env = 0.0f;
            gain = 1.0f;
            holdCounter = 0;
            midiEnv = 0.0f;
            resetDone = false;
            return 1.0f;
        }

        const float peak = std::max (std::abs (left), std::abs (right));
        env += (peak > env ? detAttack : detRelease) * (peak - env);
        midiEnv += midiDecay * (0.0f - midiEnv);

        // Soft knee from the threshold up 12 dB; zero slope at both ends.
        const float low = threshold;
        const float high = threshold * 4.0f;
        float x = juce::jlimit (0.0f, 1.0f, (env - low) / (high - low));
        x = x * x * (3.0f - 2.0f * x);
        if (source != 0)
            x = std::max (x, midiEnv);

        const float target = 1.0f - amount * x;

        if (target < gain)
        {
            gain += duckAttack * (target - gain);
            holdCounter = holdSamples;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;                        // hold the duck, then release
        }
        else
        {
            gain += release * (target - gain);
        }

        gain = juce::jlimit (0.0f, 1.0f, gain);

        // At 100% the line is wiped once, only when the duck is silent.
        if (amount > 0.999f)
        {
            if (! resetDone && gain < 0.002f && x > 0.80f)
            {
                clearNow = true;
                resetDone = true;
            }
            if (env < low * 0.5f && midiEnv < 0.01f && gain > 0.99f)
                resetDone = false;
        }

        return gain;
    }

    float getGain() const noexcept { return gain; }
    bool isActive() const noexcept { return gain < 0.999f; }

    // Samples immediately before the write head that must survive a clear:
    // this is the new note's attack plus a safety margin.
    int recentSamples() const noexcept
    {
        return std::max (1, (int) (0.012f * srate)) + 64;
    }

private:
    float srate = 44100.0f;
    bool enabled = false;
    int source = 2;
    float amount = 1.0f;
    float fadeMs = 40.0f;
    float threshold = 0.01f;
    float gain = 1.0f;
    float env = 0.0f;
    float midiEnv = 0.0f;
    int holdCounter = 0;
    int holdSamples = 0;
    float detAttack = 0.01f, detRelease = 0.001f;
    float duckAttack = 0.01f;
    float release = 0.001f;
    float midiDecay = 0.001f;
    bool resetDone = false;
};
