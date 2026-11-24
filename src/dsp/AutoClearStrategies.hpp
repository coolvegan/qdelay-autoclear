// Auto Clear behaviour strategies for QDelay (MPC and desktop builds).
//
// Both strategies share the same interface: they turn a detector peak into a
// wet gain and report when the delay line must be wiped. The engine crossfades
// between them, so switching mode while audio is running cannot click.
//
// This header is plain C++17 and has no JUCE dependency; the MPC build and the
// desktop build both include it.
#pragma once

#include <algorithm>
#include <cmath>

namespace qdelay
{

struct ClearParams
{
    bool  enabled = false;
    float fadeMs = 20.0f;
    float thresholdDb = -45.0f;
    float holdMs = 1.0f;
    float amount = 1.0f;
};

inline float clearClamp (float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Strategy interface
// ---------------------------------------------------------------------------
class ClearStrategy
{
public:
    virtual ~ClearStrategy() = default;

    virtual void prepare (float sampleRate) = 0;
    virtual void reset() = 0;
    virtual void set (const ClearParams& p) = 0;
    virtual float process (float peak, bool& clearNow) = 0;
    virtual void observeWet (float peak) = 0;
    virtual bool muteWrites() const { return false; }
    virtual const char* name() const = 0;
};

// ---------------------------------------------------------------------------
// Transient: the 1.3.1 behaviour
// ---------------------------------------------------------------------------
// Fast/slow envelope pair, a rise against the running level and a refractory
// window. It ducks once per hit and holds, then ramps back. This is the
// rhythmic ducking that dub delays want.
class TransientClear final : public ClearStrategy
{
public:
    void prepare (float sampleRate) override
    {
        srate = std::max (8000.0f, sampleRate);
        fastAttack  = 1.0f - std::exp (-1.0f / (0.001f * srate));
        fastRelease = 1.0f - std::exp (-1.0f / (0.050f * srate));
        slowAttack  = 1.0f - std::exp (-1.0f / (0.060f * srate));
        slowRelease = 1.0f - std::exp (-1.0f / (0.400f * srate));
        reset();
    }

    void reset() override
    {
        state = State::idle;
        gain = 1.0f;
        fastEnv = slowEnv = 0.0f;
        refractory = std::max (1, (int) (0.06f * srate));
        sinceTrigger = refractory;
        tailEnv = 0.0f;
        muteWrites_ = false;
    }

    void set (const ClearParams& p) override
    {
        enabled = p.enabled;
        fadeMs = clearClamp (p.fadeMs, 2.0f, 1000.0f);
        holdMs = clearClamp (p.holdMs, 0.0f, 2000.0f);
        amount = clearClamp (p.amount, 0.0f, 1.0f);
        threshold = std::pow (10.0f, clearClamp (p.thresholdDb, -80.0f, 0.0f) / 20.0f);

        if (! enabled || amount <= 0.0001f)
        {
            state = State::idle;
            gain = 1.0f;
            muteWrites_ = false;
        }
    }

    float process (float peak, bool& clearNow) override
    {
        clearNow = false;

        if (! enabled || amount <= 0.0001f)
        {
            state = State::idle;
            gain = 1.0f;
            muteWrites_ = false;
            return 1.0f;
        }

        fastEnv += (peak > fastEnv ? fastAttack : fastRelease) * (peak - fastEnv);
        slowEnv += (peak > slowEnv ? slowAttack : slowRelease) * (peak - slowEnv);

        const bool hasTail = tailEnv > 0.0005f;
        const bool aboveFloor = peak > threshold;
        const bool rising = (fastEnv > slowEnv * 1.30f) || (peak > slowEnv * 1.45f);
        if (hasTail && aboveFloor && rising && sinceTrigger > refractory && state != State::fadingOut)
            trigger();

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
                    clearNow = kill;
                    if (kill)
                        tailEnv = 0.0f;
                    held = (int) (holdMs * 0.001f * srate);
                    if (held > 0)
                    {
                        state = State::holding;
                    }
                    else
                    {
                        state = State::fadingIn;
                        prepareFadeIn();
                        muteWrites_ = false;
                    }
                }
                break;

            case State::holding:
                if (--held <= 0)
                {
                    state = State::fadingIn;
                    prepareFadeIn();
                    muteWrites_ = false;
                }
                break;

            case State::fadingIn:
                gain += fadeStep;
                if (gain >= 1.0f)
                {
                    gain = 1.0f;
                    state = State::idle;
                    muteWrites_ = false;
                }
                break;
        }

        return gain;
    }

    void observeWet (float peak) override
    {
        tailEnv += (peak > tailEnv ? 0.02f : 0.00015f) * (peak - tailEnv);
    }

    bool muteWrites() const override { return muteWrites_; }
    const char* name() const override { return "Transient"; }

private:
    enum class State { idle, fadingOut, holding, fadingIn };

    void trigger()
    {
        sinceTrigger = 0;
        kill = amount > 0.999f;
        fadeFloor = kill ? 0.0f : 1.0f - amount;
        const float outMs = kill ? 2.0f : fadeMs;
        fadeOutSamples = std::max (1, (int) (outMs * 0.001f * srate));
        fadeStep = std::max (1.0e-6f, (gain - fadeFloor) / (float) fadeOutSamples);
        if (gain <= fadeFloor)
            gain = fadeFloor;
        muteWrites_ = kill;
        state = State::fadingOut;
    }

    void prepareFadeIn()
    {
        const int samples = std::max (1, (int) (fadeMs * 0.001f * srate));
        fadeStep = std::max (1.0e-6f, (1.0f - gain) / (float) samples);
    }

    State state = State::idle;
    float srate = 44100.0f;
    bool enabled = false;
    float amount = 1.0f;
    float fadeMs = 20.0f;
    float holdMs = 1.0f;
    float threshold = 0.0056f;
    float gain = 1.0f;
    float fadeStep = 0.01f;
    float fadeFloor = 0.0f;
    int fadeOutSamples = 1;
    int held = 0;
    float fastEnv = 0.0f;
    float slowEnv = 0.0f;
    float tailEnv = 0.0f;
    float fastAttack = 0.01f, fastRelease = 0.001f;
    float slowAttack = 0.001f, slowRelease = 0.0001f;
    int sinceTrigger = 0;
    int refractory = 4000;
    bool kill = false;
    bool muteWrites_ = false;
};

// ---------------------------------------------------------------------------
// Smooth ducker: the 1.3.2 behaviour
// ---------------------------------------------------------------------------
// One envelope, a soft knee from the threshold up 12 dB, a one-pole gain and a
// hold. It follows the level continuously, so it cannot flutter at the end of
// a phrase. At 100% the line is wiped once when the duck is silent.
class SmoothDucker final : public ClearStrategy
{
public:
    void prepare (float sampleRate) override
    {
        srate = std::max (8000.0f, sampleRate);
        detAttack  = 1.0f - std::exp (-1.0f / (0.005f * srate));
        detRelease = 1.0f - std::exp (-1.0f / (0.300f * srate));
        duckAttack = 1.0f - std::exp (-1.0f / (0.005f * srate));
        reset();
    }

    void reset() override
    {
        env = 0.0f;
        gain = 1.0f;
        holdCounter = 0;
        resetDone = false;
        tailEnv = 0.0f;
    }

    void set (const ClearParams& p) override
    {
        enabled = p.enabled;
        fadeMs = clearClamp (p.fadeMs, 2.0f, 1000.0f);
        holdSamples = std::max (0, (int) (clearClamp (p.holdMs, 0.0f, 2000.0f) * 0.001f * srate));
        amount = clearClamp (p.amount, 0.0f, 1.0f);
        threshold = std::pow (10.0f, clearClamp (p.thresholdDb, -80.0f, 0.0f) / 20.0f);
        release = 1.0f - std::exp (-1.0f / (fadeMs * 0.001f * srate));

        if (! enabled || amount <= 0.0001f)
        {
            env = 0.0f;
            gain = 1.0f;
            holdCounter = 0;
            resetDone = false;
        }
    }

    float process (float peak, bool& clearNow) override
    {
        clearNow = false;

        if (! enabled || amount <= 0.0001f)
        {
            env = 0.0f;
            gain = 1.0f;
            holdCounter = 0;
            resetDone = false;
            return 1.0f;
        }

        env += (peak > env ? detAttack : detRelease) * (peak - env);

        const float low = threshold;
        const float high = threshold * 4.0f;
        float x = clearClamp ((env - low) / (high - low), 0.0f, 1.0f);
        x = x * x * (3.0f - 2.0f * x);

        const bool hasTail = tailEnv > 0.0005f;
        const float target = hasTail ? 1.0f - amount * x : 1.0f;

        if (target < gain)
        {
            gain += duckAttack * (target - gain);
            holdCounter = holdSamples;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
        }
        else
        {
            gain += release * (target - gain);
        }

        gain = clearClamp (gain, 0.0f, 1.0f);

        if (amount > 0.999f)
        {
            if (! resetDone && hasTail && gain < 0.002f && x > 0.80f)
            {
                clearNow = true;
                resetDone = true;
            }
            if (env < low * 0.5f && gain > 0.99f)
                resetDone = false;
        }

        return gain;
    }

    void observeWet (float peak) override
    {
        tailEnv += (peak > tailEnv ? 0.02f : 0.00015f) * (peak - tailEnv);
    }

    const char* name() const override { return "Ducker"; }

private:
    float srate = 44100.0f;
    bool enabled = false;
    float amount = 1.0f;
    float fadeMs = 20.0f;
    float threshold = 0.0056f;
    float gain = 1.0f;
    float env = 0.0f;
    float tailEnv = 0.0f;
    int holdCounter = 0;
    int holdSamples = 0;
    float detAttack = 0.01f, detRelease = 0.001f;
    float duckAttack = 0.01f;
    float release = 0.001f;
    bool resetDone = false;
};

// ---------------------------------------------------------------------------
// Engine: owns both strategies and crossfades between them
// ---------------------------------------------------------------------------
class AutoClearEngine
{
public:
    void prepare (float sampleRate)
    {
        srate = sampleRate > 0.0f ? sampleRate : 44100.0f;
        transient.prepare (srate);
        ducker.prepare (srate);
        fadeStep = 1.0f / std::max (1.0f, 0.025f * srate);   // 25 ms
        fbStep   = 1.0f - std::exp (-1.0f / (0.020f * srate));
        reset();
    }

    void reset()
    {
        transient.reset();
        ducker.reset();
        active = &transient;
        previous = nullptr;
        fade = 1.0f;
        gain = 1.0f;
        fbMix = 0.0f;
    }

    // smooth = false: Transient, true: Ducker.
    void set (const ClearParams& p, bool smooth, bool feedback)
    {
        transient.set (p);
        ducker.set (p);

        ClearStrategy* want = smooth ? static_cast<ClearStrategy*> (&ducker)
                                     : static_cast<ClearStrategy*> (&transient);
        if (want != active)
        {
            previous = active;
            active = want;
            fade = 0.0f;
        }

        wantFeedback = feedback;
    }

    float process (float peak, bool& clearNow)
    {
        bool cActive = false;
        bool cPrev = false;
        (void) cPrev;

        const float gActive = active->process (peak, cActive);
        float out = gActive;

        if (previous != nullptr && fade < 1.0f)
        {
            const float gPrev = previous->process (peak, cPrev);
            out = gPrev * (1.0f - fade) + gActive * fade;
            fade += fadeStep;
            if (fade >= 1.0f)
            {
                fade = 1.0f;
                previous = nullptr;
            }
        }

        clearNow = cActive;     // never let the outgoing strategy wipe the line
        gain = out;

        const float fbTarget = wantFeedback ? 1.0f : 0.0f;
        fbMix += fbStep * (fbTarget - fbMix);
        return out;
    }

    void observeWet (float peak)
    {
        transient.observeWet (peak);
        ducker.observeWet (peak);
    }

    // 1 = no feedback scaling, gain < 1 ducks the loop.
    float feedbackScale() const { return 1.0f - fbMix * (1.0f - gain); }

    bool muteWrites() const { return active->muteWrites(); }

    float getGain() const { return gain; }

private:
    TransientClear transient;
    SmoothDucker ducker;
    ClearStrategy* active = &transient;
    ClearStrategy* previous = nullptr;
    float srate = 44100.0f;
    float fade = 1.0f;
    float fadeStep = 0.001f;
    float gain = 1.0f;
    float fbMix = 0.0f;
    float fbStep = 0.001f;
    bool wantFeedback = false;
};

} // namespace qdelay
