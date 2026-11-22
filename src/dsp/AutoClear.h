// Copyright 2025 tilr
// Auto Clear - smooth, note-triggered delay tail termination (Air Delay Pro style)
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

/**
    Auto Clear watches the dry input (and optionally incoming MIDI) for a new note
    and fades the wet signal out, clears the delay line, then fades the wet signal
    back in. Unlike a hard reset this cannot click: the line is only cleared once
    the wet path has reached its (configurable) minimum gain.

    The detector is deliberately simple and robust for live use: a fast/slow
    envelope pair finds transients, with a refractory window so a single hit or a
    sustained pad only triggers once. MIDI note-ons can trigger it as well.
*/
class AutoClear
{
public:
    AutoClear() = default;

    void prepare (double sampleRate)
    {
        srate = (float) std::max (8000.0, sampleRate);
        fastAttack  = 1.0f - std::exp (-1.0f / (0.001f  * srate));
        fastRelease = 1.0f - std::exp (-1.0f / (0.050f  * srate));
        slowAttack  = 1.0f - std::exp (-1.0f / (0.060f  * srate));
        slowRelease = 1.0f - std::exp (-1.0f / (0.400f  * srate));
        reset();
    }

    void reset()
    {
        state = State::idle;
        gain = 1.0f;
        fastEnv = 0.0f;
        slowEnv = 0.0f;
        sinceTrigger = 0;
        refractorySamples = std::max (1, (int) (0.09f * srate));
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
        holdMs = juce::jlimit (0.0f, 2000.0f, holdMs_);
        amount = juce::jlimit (0.0f, 1.0f, amount_);
        threshold = std::pow (10.0f, juce::jlimit (-80.0f, 0.0f, thresholdDb_) / 20.0f);
        source = juce::jlimit (0, 2, source_); // 0 audio, 1 midi, 2 both

        if (! enabled || amount <= 0.0001f)
        {
            state = State::idle;
            gain = 1.0f;
        }
    }

    /** Call once at the start of each block. */
    void beginBlock (bool midiNoteOn)
    {
        if (enabled && amount > 0.0001f && midiNoteOn && (source == 1 || source == 2))
            trigger();
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
            state = State::idle;
            gain = 1.0f;
            return 1.0f;
        }

        const float peak = std::max (std::abs (left), std::abs (right));

        // transient detector
        fastEnv += (peak > fastEnv ? fastAttack : fastRelease) * (peak - fastEnv);
        slowEnv += (peak > slowEnv ? slowAttack : slowRelease) * (peak - slowEnv);

        if (source == 0 || source == 2)
        {
            const bool above = fastEnv > threshold || peak > threshold;
            const bool transient = above
                                && fastEnv > slowEnv * 2.2f
                                && sinceTrigger > refractorySamples;

            if (transient && state != State::fadingOut)
                trigger();
        }

        if (sinceTrigger < 0x3fffffff)
            ++sinceTrigger;

        switch (state)
        {
            case State::idle:
                gain = 1.0f;
                break;

            case State::fadingOut:
                gain -= fadeStep;
                if (gain <= fadeFloor)
                {
                    gain = fadeFloor;
                    // clearing at minimum gain hides the line reset completely
                    clearNow = amount > 0.999f;
                    held = (int) (holdMs * 0.001f * srate);
                    state = held > 0 ? State::holding : State::fadingIn;
                    if (state == State::fadingIn)
                        prepareFadeIn();
                }
                break;

            case State::holding:
                if (--held <= 0)
                {
                    state = State::fadingIn;
                    prepareFadeIn();
                }
                break;

            case State::fadingIn:
                gain += fadeStep;
                if (gain >= 1.0f)
                {
                    gain = 1.0f;
                    state = State::idle;
                }
                break;
        }

        return gain;
    }

    float getGain() const noexcept { return gain; }
    bool isActive() const noexcept { return state != State::idle; }

    // Samples immediately before the write head that must survive a clear:
    // this is the new note's attack plus a safety margin.
    int recentSamples() const noexcept { return fadeOutSamples + 64; }

private:
    enum class State { idle, fadingOut, holding, fadingIn };

    void trigger()
    {
        sinceTrigger = 0;
        fadeFloor = 1.0f - amount;
        // Full clears fade out quickly (the new attack is preserved), then
        // fade back in over the user's fade time. Partial ducks use the full
        // fade time in both directions.
        const float outMs = amount > 0.999f ? std::min (fadeMs, 12.0f) : fadeMs;
        fadeOutSamples = std::max (1, (int) (outMs * 0.001f * srate));
        fadeStep = std::max (1.0e-6f, (gain - fadeFloor) / (float) fadeOutSamples);
        if (gain <= fadeFloor)
            gain = fadeFloor;
        state = State::fadingOut;
    }

    void prepareFadeIn()
    {
        const int samples = std::max (1, (int) (fadeMs * 0.001f * srate));
        fadeStep = std::max (1.0e-6f, (1.0f - gain) / (float) samples);
        if (gain >= 1.0f)
            gain = 1.0f;
    }

    State state = State::idle;
    float srate = 44100.0f;
    bool enabled = false;
    int source = 2;
    float amount = 1.0f;
    float fadeMs = 40.0f;
    float holdMs = 0.0f;
    float threshold = 0.01f;
    float gain = 1.0f;
    float fadeStep = 0.01f;
    float fadeFloor = 0.0f;
    int fadeOutSamples = 1;
    int held = 0;
    float fastEnv = 0.0f;
    float slowEnv = 0.0f;
    float fastAttack = 0.01f, fastRelease = 0.001f;
    float slowAttack = 0.001f, slowRelease = 0.0001f;
    int sinceTrigger = 0;
    int refractorySamples = 4000;
};
