// Copyright 2025 tilr
// Auto Clear - smooth, note-triggered delay tail termination (Air Delay Pro style)
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

#include "AutoClearStrategies.hpp"

/**
    Desktop wrapper around the shared Auto Clear strategies.

    The audio logic lives in qdelay::AutoClearEngine, which is also used by the
    MPC build. This class only adds the MIDI source: a note-on feeds a decaying
    envelope into the detector, so MIDI-only triggering works.
*/
class AutoClear
{
public:
    AutoClear() = default;

    void prepare (double sampleRate)
    {
        srate = (float) std::max (8000.0, sampleRate);
        midiDecay = 1.0f - std::exp (-1.0f / (0.300f * srate));
        engine.prepare (srate);
        reset();
    }

    void reset()
    {
        engine.reset();
        midiEnv = 0.0f;
    }

    void setParameters (bool enabled_,
                        float fadeMs_,
                        float thresholdDb_,
                        float holdMs_,
                        float amount_,
                        int source_,
                        int mode_ = 0)
    {
        source = juce::jlimit (0, 2, source_);
        mode = juce::jlimit (0, 1, mode_);

        qdelay::ClearParams p;
        p.enabled = enabled_;
        p.fadeMs = fadeMs_;
        p.thresholdDb = thresholdDb_;
        p.holdMs = holdMs_;
        p.amount = amount_;
        engine.set (p, mode == 1, false);
    }

    /** Call once at the start of each block. */
    void beginBlock (bool midiNoteOn)
    {
        if (midiNoteOn && (source == 1 || source == 2))
            midiEnv = 1.0f;
    }

    /**
        Per-sample update.

        @returns the wet gain for this sample. When @p clearNow is set the caller
                 must clear the delay line before the next sample is processed.
    */
    float process (float left, float right, bool& clearNow)
    {
        midiEnv += midiDecay * (0.0f - midiEnv);

        float peak = std::max (std::abs (left), std::abs (right));
        if (source != 0)
            peak = std::max (peak, midiEnv);

        return engine.process (peak, clearNow);
    }

    void observeWet (float peak) { engine.observeWet (peak); }

    float getGain() const noexcept { return engine.getGain(); }
    bool isActive() const noexcept { return engine.getGain() < 0.999f; }

    int recentSamples() const noexcept
    {
        return std::max (1, (int) (0.012f * srate)) + 64;
    }

private:
    qdelay::AutoClearEngine engine;
    float srate = 44100.0f;
    float midiEnv = 0.0f;
    float midiDecay = 0.001f;
    int source = 2;
    int mode = 0;
};
