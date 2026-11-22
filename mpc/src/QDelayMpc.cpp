// QDelay for Akai MPC standalone (VST2 / AArch64)
//
// A pro-grade stereo dub delay: independent tempo-synced taps, ping-pong and
// classic ping-pong routing, filtered and saturated feedback, diffusion,
// modulation, tape wow/flutter, ducking and an "Auto Clear" tail fade that is
// triggered by new notes (the AIR Delay Pro Auto Clear behaviour, but smooth).
//
// Built on the MIT-licensed MPC Plugin SDK framework. No JUCE and no X11:
// MPC plugins have no native windows, the on-screen UI is the JSON skin in
// Plugin Skins/.
//
// SPDX-License-Identifier: MIT

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/delay_line.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace mpc::dsp;
using mpc::boolParam;
using mpc::choiceParam;
using mpc::floatParam;
using mpc::logParam;

namespace
{

// ---------------------------------------------------------------------------
// Auto Clear
// ---------------------------------------------------------------------------
class AutoClear
{
public:
    void prepare (double sampleRate)
    {
        srate = (float) std::max (8000.0, sampleRate);
        fastAttack  = 1.0f - std::exp (-1.0f / (0.001f * srate));
        fastRelease = 1.0f - std::exp (-1.0f / (0.050f * srate));
        slowAttack  = 1.0f - std::exp (-1.0f / (0.060f * srate));
        slowRelease = 1.0f - std::exp (-1.0f / (0.400f * srate));
        reset();
    }

    void reset()
    {
        state = State::idle;
        gain = 1.0f;
        fastEnv = slowEnv = 0.0f;
        refractory = std::max (1, (int) (0.06f * srate));
        sinceTrigger = refractory;   // the very first note may trigger immediately
        tailEnv = 0.0f;
        muteWrites = false;
    }

    // MPC effects never receive MIDI: Auto Clear is always audio-triggered.
    void set (bool enabled_, float fadeMs_, float thresholdDb_, float holdMs_, float amount_)
    {
        enabled = enabled_;
        fadeMs = clamp (fadeMs_, 2.0f, 1000.0f);
        holdMs = clamp (holdMs_, 0.0f, 2000.0f);
        amount = clamp (amount_, 0.0f, 1.0f);
        threshold = std::pow (10.0f, clamp (thresholdDb_, -80.0f, 0.0f) / 20.0f);

        if (! enabled || amount <= 0.0001f)
        {
            state = State::idle;
            gain = 1.0f;
            muteWrites = false;
        }
    }

    /**
        Returns the wet gain for this sample.

        clearNow  - set once when a 100% clear must wipe the whole delay line.
        muteWrites- set while the kill is active, so the caller writes zeros
                    into the delay/feedback loop instead of refilling it.
    */
    float process (float left, float right, bool& clearNow, bool& muteWrites_)
    {
        clearNow = false;
        muteWrites_ = false;

        if (! enabled || amount <= 0.0001f)
        {
            state = State::idle;
            gain = 1.0f;
            muteWrites = false;
            return 1.0f;
        }

        const float peak = std::max (std::abs (left), std::abs (right));
        fastEnv += (peak > fastEnv ? fastAttack : fastRelease) * (peak - fastEnv);
        slowEnv += (peak > slowEnv ? slowAttack : slowRelease) * (peak - slowEnv);

        // Adaptive onset detector. The absolute threshold is only a floor;
        // the real trigger is a rise against the running level. This works
        // for a new note over a sustained pad or a busy mix, not just for a
        // clean transient in silence.
        // Only kill when there is actually a delay tail to kill. This keeps
        // the first note after loading from erasing its own first repeat.
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
                        muteWrites = false;
                    }
                }
                break;

            case State::holding:
                if (--held <= 0)
                {
                    state = State::fadingIn;
                    prepareFadeIn();
                    muteWrites = false;
                }
                break;

            case State::fadingIn:
                gain += fadeStep;
                if (gain >= 1.0f)
                {
                    gain = 1.0f;
                    state = State::idle;
                    muteWrites = false;
                }
                break;
        }

        muteWrites_ = muteWrites;
        return gain;
    }

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
        muteWrites = kill;
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
    bool muteWrites = false;
public:
    // Called by the processor with the delay output so Auto Clear knows
    // whether there is a tail worth killing.
    void observeWet (float peak)
    {
        tailEnv += (peak > tailEnv ? 0.02f : 0.00015f) * (peak - tailEnv);
    }
};

// ---------------------------------------------------------------------------
// Diffusion allpass
// ---------------------------------------------------------------------------
class Allpass
{
public:
    void prepare (int maxSamples) { line.prepare (maxSamples); }
    void reset() { line.reset(); }
    void setLength (float samples) { length = clamp (samples, 1.0f, (float) line.maxDelay()); }
    void setGain (float g_) { g = clamp (g_, 0.0f, 0.85f); }

    float process (float x)
    {
        const float d = line.readAllpass (length);
        const float v = x + g * d;
        line.write (v);
        return d - g * v;
    }

private:
    DelayLine line;
    float length = 113.0f;
    float g = 0.5f;
};

class Diffuser
{
public:
    static constexpr int kStages = 4;
    static constexpr float kBaseLengths[kStages] = { 113.0f, 163.0f, 241.0f, 337.0f };

    void prepare (double sr)
    {
        srate = (float) sr;
        const float scale = srate / 44100.0f;
        for (int i = 0; i < kStages; ++i)
            ap[(size_t) i].prepare ((int) (kBaseLengths[i] * 2.2f * scale) + 16);
        size.setTime (90.0f, sr);
        size.snap (0.5f);
        reset();
    }

    void reset()
    {
        for (auto& a : ap)
            a.reset();
    }

    // Size is smoothed per sample; the allpass lengths glide instead of jumping
    // when the SIZE knob is turned, so there are no clicks.
    void setSize (float size01) { size.setTarget (clamp (size01, 0.0f, 1.0f)); }

    // 100% allpass output: never mix a phase-rotated allpass with the dry
    // signal, which would comb-filter and sound hollow/metallic.
    float process (float x, float amount)
    {
        if (amount <= 0.0001f)
            return x;

        const float dry = x;
        const float s = size.process();
        const float scale = srate / 44100.0f;
        const float factor = 0.45f + 1.55f * s;
        const float g = 0.5f + 0.3f * amount;

        for (int i = 0; i < kStages; ++i)
        {
            ap[(size_t) i].setLength (kBaseLengths[i] * factor * scale);
            ap[(size_t) i].setGain (g);
            x = ap[(size_t) i].process (x);
        }

        // Very small amounts crossfade in over the first 5% so switching the
        // diffuser on cannot click; above that it is 100% allpass (no comb).
        const float blend = clamp (amount * 20.0f, 0.0f, 1.0f);
        return x * blend + dry * (1.0f - blend);
    }

private:
    std::array<Allpass, kStages> ap;
    Smoother size;
    float srate = 44100.0f;
};


// ---------------------------------------------------------------------------
// Envelope follower for the duck control
// ---------------------------------------------------------------------------
class DuckFollower
{
public:
    void prepare (double sr)
    {
        srate = (float) sr;
        reset();
    }

    void reset()
    {
        env = 0.0f;
    }

    void set (float thresholdDb, float attackMs, float releaseMs)
    {
        threshold = std::pow (10.0f, clamp (thresholdDb, -80.0f, 0.0f) / 20.0f);
        attack = 1.0f - std::exp (-1.0f / std::max (0.05f, attackMs) * 0.001f * srate);
        release = 1.0f - std::exp (-1.0f / std::max (1.0f, releaseMs) * 0.001f * srate);
    }

    float process (float left, float right)
    {
        const float peak = std::max (std::abs (left), std::abs (right));
        const float input = std::max (0.0f, peak - threshold);
        env += (input > env ? attack : release) * (input - env);
        return env;
    }

private:
    float srate = 44100.0f;
    float threshold = 0.01f;
    float attack = 0.1f, release = 0.001f;
    float env = 0.0f;
};

// Transparent below -0.9 dBFS, gently limits the last 0.9 dB so the plugin
// can never send > 0 dBFS into the MPC mixer.
inline float softLimit (float x)
{
    constexpr float knee = 0.9f;
    const float a = std::fabs (x);
    if (a <= knee)
        return x;
    const float y = knee + (1.0f - knee) * fastTanh ((a - knee) / (1.0f - knee));
    return x < 0.0f ? -y : y;
}

// ---------------------------------------------------------------------------
// 2x oversampled feedback saturation
// ---------------------------------------------------------------------------
// A 15-tap halfband FIR upsamples, saturates at 2x and downsamples again.
// This keeps tanh harmonics from folding back into the audible band, which was
// the main source of the harsh, "fizzy" feedback on the first MPC build.
class Sat2x
{
public:
    static constexpr int N = 15;
    static constexpr int Ring = 32;

    void prepare (double sampleRate)
    {
        (void) sampleRate;
        const int M = N / 2;
        double sum = 0.0;
        for (int n = 0; n < N; ++n)
        {
            const double x = (double) (n - M);
            const double sinc = std::fabs (x) < 1.0e-9 ? 1.0
                                 : std::sin (kPi * 0.5 * x) / (kPi * 0.5 * x);
            const double w = 0.42
                           - 0.5 * std::cos (2.0 * kPi * n / (N - 1))
                           + 0.08 * std::cos (4.0 * kPi * n / (N - 1));
            h[n] = (float) (0.5 * sinc * w);
            sum += h[n];
        }
        if (sum > 1.0e-9)
            for (float& v : h)
                v = (float) (v / sum);
        reset();
    }

    void reset()
    {
        up.fill (0.0f);
        down.fill (0.0f);
        write = 0;
    }

    float process (float x, float drive)
    {
        // halfband interpolation (zero-stuff + FIR)
        up[(size_t) write] = x;
        up[(size_t) ((write + 1) & (Ring - 1))] = 0.0f;

        float y0 = 0.0f, y1 = 0.0f;
        for (int k = 0; k < N; ++k)
        {
            const int a = (write - k) & (Ring - 1);
            const int b = (write + 1 - k) & (Ring - 1);
            y0 += h[k] * up[(size_t) a];
            y1 += h[k] * up[(size_t) b];
        }

        // Zero-stuffing halves the average level; the interpolation filter
        // has unity DC gain, so compensate by 2 before saturating.
        y0 *= 2.0f;
        y1 *= 2.0f;

        const float g = 1.0f + drive * 4.0f;
        const float norm = fastTanh (g);
        const float mix = 0.7f * drive;
        const auto sat = [&] (float v)
        {
            return v * (1.0f - mix) + (fastTanh (v * g) / norm) * mix;
        };

        down[(size_t) write] = sat (y0);
        down[(size_t) ((write + 1) & (Ring - 1))] = sat (y1);

        // decimation (FIR, take the odd phase)
        float out = 0.0f;
        for (int k = 0; k < N; ++k)
            out += h[k] * down[(size_t) ((write + 1 - k) & (Ring - 1))];

        write = (write + 2) & (Ring - 1);
        return out;
    }

private:
    std::array<float, N> h{};
    std::array<float, Ring> up{};
    std::array<float, Ring> down{};
    int write = 0;
};

} // namespace

// ===========================================================================
// The plugin
// ===========================================================================
class QDelayMpc : public mpc::Plugin
{
public:
    enum P
    {
        pSync, pDivL, pDivR, pTimeL, pTimeR, pFeedback, pMix, pFreeze,
        pMode, pWidth, pStereo, pSwing, pFeel, pTape, pOutput,
        pLowCut, pHighCut, pDrive, pTone, pDiffAmt, pDiffSize, pModDepth, pModRate,
        pEqLow, pEqMid, pEqHigh, pDuckAmt, pDuckThres, pDuckAtk, pDuckRel,
        pAutoClear, pClearFade, pClearThresh, pClearHold, pClearAmt,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "QDelay";
        i.vendor = "Tilr";
        i.uniqueId = MPC_FOURCC ('q', 'd', 'e', 'l');
        i.version = 1301;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.wantsMidi = false;       // MPC effects never receive MIDI
        return i;
    }

    QDelayMpc() : mpc::Plugin (makeInfo())
    {
        addParam (boolParam ("sync", "Sync", true));
        addParam (choiceParam ("div_l", "Div L", divisionNames(), kDivisionEighth));
        addParam (choiceParam ("div_r", "Div R", divisionNames(), kDivisionEighth));
        addParam (logParam ("time_l", "Time L", 10.0f, 4000.0f, 375.0f, "ms", 0));
        addParam (logParam ("time_r", "Time R", 10.0f, 4000.0f, 375.0f, "ms", 0));
        addParam (floatParam ("feedback", "Feedback", 0.0f, 110.0f, 55.0f, "%", 0));
        addParam (floatParam ("mix", "Mix", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam (boolParam ("freeze", "Freeze", false));

        addParam (choiceParam ("mode", "Mode", { "Normal", "Ping-Pong", "Classic" }, 0));
        addParam (floatParam ("width", "Width", -100.0f, 100.0f, 100.0f, "%", 0));
        addParam (floatParam ("stereo", "Stereo", 0.0f, 200.0f, 100.0f, "%", 0));
        addParam (floatParam ("swing", "Swing", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("feel", "Feel", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("tape", "Tape", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("output", "Output", -18.0f, 18.0f, 0.0f, "dB", 1));

        addParam (logParam ("lowcut", "Low Cut", 20.0f, 2000.0f, 60.0f, "Hz", 0));
        addParam (logParam ("highcut", "High Cut", 500.0f, 20000.0f, 12000.0f, "Hz", 0));
        addParam (floatParam ("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("tone", "Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam (floatParam ("diff_amt", "Diff Amt", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("diff_size", "Diff Size", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam (floatParam ("mod_depth", "Mod Depth", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam (logParam ("mod_rate", "Mod Rate", 0.01f, 10.0f, 0.5f, "Hz", 2));

        addParam (floatParam ("eq_low", "EQ Low", -18.0f, 18.0f, 0.0f, "dB", 1));
        addParam (floatParam ("eq_mid", "EQ Mid", -18.0f, 18.0f, 0.0f, "dB", 1));
        addParam (floatParam ("eq_high", "EQ High", -18.0f, 18.0f, 0.0f, "dB", 1));

        addParam (floatParam ("duck_amt", "Duck Amt", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam (floatParam ("duck_thr", "Duck Thr", -60.0f, 0.0f, -30.0f, "dB", 1));
        addParam (logParam ("duck_atk", "Duck Atk", 0.1f, 200.0f, 5.0f, "ms", 1));
        addParam (logParam ("duck_rel", "Duck Rel", 10.0f, 5000.0f, 300.0f, "ms", 0));

        addParam (boolParam ("autoclear", "Auto Clear", true));
        addParam (logParam ("clear_fade", "Clear Fade", 2.0f, 500.0f, 20.0f, "ms", 0));
        addParam (floatParam ("clear_thr", "Clear Thr", -60.0f, 0.0f, -45.0f, "dB", 1));
        addParam (logParam ("clear_hold", "Clear Hold", 1.0f, 1000.0f, 1.0f, "ms", 0));
        addParam (floatParam ("clear_amt", "Clear Amt", 0.0f, 100.0f, 100.0f, "%", 0));

        addPrograms();
    }

    void prepare (double sampleRate, int maxBlockSize) override
    {
        (void) maxBlockSize;
        const int maxDelay = (int) (sampleRate * 4.0) + 8;
        delayL.prepare (maxDelay);
        delayR.prepare (maxDelay);

        diffL.prepare (sampleRate);
        diffR.prepare (sampleRate);

        timeL.setTime (160.0f, sampleRate);
        timeR.setTime (160.0f, sampleRate);
        feedback.setTime (20.0f, sampleRate);
        mix.setTime (25.0f, sampleRate);
        width.setTime (20.0f, sampleRate);
        stereo.setTime (20.0f, sampleRate);
        drive.setTime (20.0f, sampleRate);
        output.setTime (20.0f, sampleRate);
        modDepth.setTime (30.0f, sampleRate);
        diffAmt.setTime (30.0f, sampleRate);
        tapeSmooth.setTime (25.0f, sampleRate);
        duckAmtSmooth.setTime (20.0f, sampleRate);
        lowCutSmooth.setTime (30.0f, sampleRate);
        highCutSmooth.setTime (30.0f, sampleRate);
        toneSmooth.setTime (30.0f, sampleRate);
        freezeSmooth.setTime (25.0f, sampleRate);
        eqLowSmooth.setTime (30.0f, sampleRate);
        eqMidSmooth.setTime (30.0f, sampleRate);
        eqHighSmooth.setTime (30.0f, sampleRate);

        lowCutL.setCoeffs (BiquadCoeffs::highpass (sampleRate, 60.0f, 0.707f));
        lowCutR.setCoeffs (BiquadCoeffs::highpass (sampleRate, 60.0f, 0.707f));
        highCutL.setCoeffs (BiquadCoeffs::lowpass (sampleRate, 12000.0f, 0.707f));
        highCutR.setCoeffs (BiquadCoeffs::lowpass (sampleRate, 12000.0f, 0.707f));
        sendLowL.setCoeffs (BiquadCoeffs::highpass (sampleRate, 60.0f, 0.707f));
        sendLowR.setCoeffs (BiquadCoeffs::highpass (sampleRate, 60.0f, 0.707f));
        sendHighL.setCoeffs (BiquadCoeffs::lowpass (sampleRate, 12000.0f, 0.707f));
        sendHighR.setCoeffs (BiquadCoeffs::lowpass (sampleRate, 12000.0f, 0.707f));
        satToneL.setCutoff (8000.0f, sampleRate);
        satToneR.setCutoff (8000.0f, sampleRate);

        dcL.prepare (sampleRate, 8.0f);
        dcR.prepare (sampleRate, 8.0f);
        autoClear.prepare (sampleRate);
        duck.prepare (sampleRate);

        const float sr = (float) sampleRate;
        maxDelaySamples = (float) (maxDelay - 4);
        modPhase = wowPhase = flutterPhase = 0.0f;
        reset();
    }

    void reset() override
    {
        delayL.reset();
        delayR.reset();
        diffL.reset();
        diffR.reset();

        lowCutL.reset(); lowCutR.reset();
        highCutL.reset(); highCutR.reset();
        sendLowL.reset(); sendLowR.reset();
        sendHighL.reset(); sendHighR.reset();
        satToneL.reset(); satToneR.reset();
        satL.reset(); satR.reset();
        eqLowL.reset(); eqLowR.reset();
        eqMidL.reset(); eqMidR.reset();
        eqHighL.reset(); eqHighR.reset();
        dcL.reset(); dcR.reset();

        autoClear.reset();
        duck.reset();

        timeL.snap (375.0f);
        timeR.snap (375.0f);
        feedback.snap (0.55f);
        mix.snap (0.35f);
        width.snap (1.0f);
        stereo.snap (1.0f);
        drive.snap (0.0f);
        output.snap (1.0f);
        modDepth.snap (0.0f);
        diffAmt.snap (0.0f);
        tapeSmooth.snap (clamp (get (pTape) * 0.01f, 0.0f, 1.0f));
        duckAmtSmooth.snap (clamp (get (pDuckAmt) * 0.01f, 0.0f, 1.0f));
        lowCutSmooth.snap (get (pLowCut));
        highCutSmooth.snap (get (pHighCut));
        toneSmooth.snap (clamp (get (pTone) * 0.01f, 0.0f, 1.0f));
        freezeSmooth.snap (getBool (pFreeze) ? 1.0f : 0.0f);
        eqLowSmooth.snap (get (pEqLow));
        eqMidSmooth.snap (get (pEqMid));
        eqHighSmooth.snap (get (pEqHigh));
    }

    void process (mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0)
            return;

        const float* inL = ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* outL = ctx.numOutputs > 0 ? ctx.outputs[0] : nullptr;
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : outL;
        if (! outL)
            return;

        const float sr = (float) ctx.sampleRate;
        const float bpm = safeBpm (ctx.transport.bpm);

        // ---- per-block parameter targets --------------------------------
        const bool sync = getBool (pSync);
        const bool freeze = getBool (pFreeze);
        const int mode = getInt (pMode);

        float targetL = sync
            ? beatsToSamples (divisionBeats (getInt (pDivL)), bpm, sr)
            : msToSamples (get (pTimeL), sr);
        float targetR = sync
            ? beatsToSamples (divisionBeats (getInt (pDivR)), bpm, sr)
            : msToSamples (get (pTimeR), sr);

        const float swing = get (pSwing) * 0.01f;
        float feel = get (pFeel) * 0.01f;
        targetR *= 1.0f + 0.5f * swing;
        targetL += feel * 0.06f * sr;
        targetR += feel * 0.06f * sr;

        timeL.setTarget (clamp (targetL, 1.0f, maxDelaySamples));
        timeR.setTarget (clamp (targetR, 1.0f, maxDelaySamples));

        feedback.setTarget (clamp (get (pFeedback) * 0.01f, 0.0f, 0.995f));
        mix.setTarget (clamp (get (pMix) * 0.01f, 0.0f, 1.0f));
        width.setTarget (clamp (get (pWidth) * 0.01f, -1.0f, 1.0f));
        stereo.setTarget (clamp (get (pStereo) * 0.01f, 0.0f, 2.0f));
        drive.setTarget (clamp (get (pDrive) * 0.01f, 0.0f, 1.0f));
        output.setTarget (dbToGain (get (pOutput)));
        modDepth.setTarget (clamp (get (pModDepth) * 0.01f, 0.0f, 1.0f));
        diffAmt.setTarget (clamp (get (pDiffAmt) * 0.01f, 0.0f, 1.0f));
        tapeSmooth.setTarget (clamp (get (pTape) * 0.01f, 0.0f, 1.0f));
        duckAmtSmooth.setTarget (clamp (get (pDuckAmt) * 0.01f, 0.0f, 1.0f));
        lowCutSmooth.setTarget (get (pLowCut));
        highCutSmooth.setTarget (get (pHighCut));
        toneSmooth.setTarget (clamp (get (pTone) * 0.01f, 0.0f, 1.0f));
        freezeSmooth.setTarget (freeze ? 1.0f : 0.0f);
        eqLowSmooth.setTarget (get (pEqLow));
        eqMidSmooth.setTarget (get (pEqMid));
        eqHighSmooth.setTarget (get (pEqHigh));

        // filters and EQ (12 dB/oct low/high cut, smoothed to avoid zipper)
        const float lc = lowCutSmooth.process();
        const float hc = highCutSmooth.process();
        const auto lowCutCoeffs  = BiquadCoeffs::highpass (sr, lc, 0.707f);
        const auto highCutCoeffs = BiquadCoeffs::lowpass (sr, hc, 0.707f);
        lowCutL.setCoeffs (lowCutCoeffs);   lowCutR.setCoeffs (lowCutCoeffs);
        highCutL.setCoeffs (highCutCoeffs); highCutR.setCoeffs (highCutCoeffs);
        sendLowL.setCoeffs (lowCutCoeffs);   sendLowR.setCoeffs (lowCutCoeffs);
        sendHighL.setCoeffs (highCutCoeffs); sendHighR.setCoeffs (highCutCoeffs);

        const float tone = toneSmooth.process();
        const float satCutoff = 1000.0f + 19000.0f * tone;
        satToneL.setCutoff (satCutoff, sr);
        satToneR.setCutoff (satCutoff, sr);

        const auto lowCoeffs  = BiquadCoeffs::lowShelf (sr, 120.0f, 0.707f, eqLowSmooth.process());
        const auto midCoeffs  = BiquadCoeffs::peak (sr, 800.0f, 0.9f, eqMidSmooth.process());
        const auto highCoeffs = BiquadCoeffs::highShelf (sr, 4500.0f, 0.707f, eqHighSmooth.process());
        eqLowL.setCoeffs (lowCoeffs);   eqLowR.setCoeffs (lowCoeffs);
        eqMidL.setCoeffs (midCoeffs);   eqMidR.setCoeffs (midCoeffs);
        eqHighL.setCoeffs (highCoeffs); eqHighR.setCoeffs (highCoeffs);

        const float diffSize = clamp (get (pDiffSize) * 0.01f, 0.0f, 1.0f);
        diffL.setSize (diffSize);
        diffR.setSize (diffSize);

        const float modRate = get (pModRate);
        const float modInc = modRate / sr;
        const float wowInc = 0.35f / sr;
        const float flutterInc = 6.2f / sr;

        duck.set (get (pDuckThres), get (pDuckAtk), get (pDuckRel));

        autoClear.set (getBool (pAutoClear),
                       get (pClearFade),
                       get (pClearThresh),
                       get (pClearHold),
                       clamp (get (pClearAmt) * 0.01f, 0.0f, 1.0f));

        // ---- per-sample loop --------------------------------------------
        for (int i = 0; i < n; ++i)
        {
            const float dryL = inL ? inL[i] : 0.0f;
            const float dryR = inR ? inR[i] : 0.0f;

            // Auto Clear must be evaluated before the delay write so a full
            // kill can mute the loop and wipe the line at zero gain.
            bool clearNow = false, muteWrites = false;
            const float autoClearGain = autoClear.process (dryL, dryR, clearNow, muteWrites);
            if (clearNow)
            {
                // Full kill: wipe the line and every stateful block around it,
                // otherwise a long IIR tail could refill the loop at max feedback.
                delayL.reset();
                delayR.reset();
                diffL.reset();
                diffR.reset();
                lowCutL.reset(); lowCutR.reset();
                highCutL.reset(); highCutR.reset();
                sendLowL.reset(); sendLowR.reset();
                sendHighL.reset(); sendHighR.reset();
                eqLowL.reset(); eqLowR.reset();
                eqMidL.reset(); eqMidR.reset();
                eqHighL.reset(); eqHighR.reset();
                satToneL.reset(); satToneR.reset();
                satL.reset(); satR.reset();
                dcL.reset(); dcR.reset();
            }

            const float lfo = fastSin01 (modPhase);
            modPhase = wrapPhase (modPhase + modInc);
            const float wow = fastSin01 (wowPhase) * 0.6f + fastSin01 (flutterPhase) * 0.15f;
            wowPhase = wrapPhase (wowPhase + wowInc);
            flutterPhase = wrapPhase (flutterPhase + flutterInc);

            const float md = modDepth.process();
            const float tapeAmt = tapeSmooth.process();
            const float modSamples = md * 0.0035f * sr;
            const float tapeSamples = tapeAmt * 0.0012f * sr;

            const float dL = clamp (timeL.process() + lfo * modSamples + wow * tapeSamples,
                                    1.0f, maxDelaySamples);
            const float dR = clamp (timeR.process() - lfo * modSamples + wow * tapeSamples,
                                    1.0f, maxDelaySamples);

            float wetL = delayL.readLinear (dL);
            float wetR = delayR.readLinear (dR);
            autoClear.observeWet (std::max (std::abs (wetL), std::abs (wetR)));

            // feedback shaping: 12 dB filters, EQ, then gentle saturation.
            float fbL = lowCutL.process (wetL);
            float fbR = lowCutR.process (wetR);
            fbL = highCutL.process (fbL);
            fbR = highCutR.process (fbR);
            fbL = eqHighL.process (eqMidL.process (eqLowL.process (fbL)));
            fbR = eqHighR.process (eqMidR.process (eqLowR.process (fbR)));

            const float drv = drive.process();
            if (drv > 0.001f)
            {
                fbL = satToneL.process (satL.process (fbL, drv));
                fbR = satToneR.process (satR.process (fbR, drv));
            }

            fbL = dcL.process (fbL);
            fbR = dcR.process (fbR);

            const float fz = freezeSmooth.process();
            float fb = feedback.process();
            fb = fb + (0.995f - fb) * fz;

            // Diffusion: allpass only. Mixing the phase-rotated allpass with the
            // dry signal would create comb notches (the old hollow/metallic bug).
            float sendL = sendHighL.process (sendLowL.process (dryL));
            float sendR = sendHighR.process (sendLowR.process (dryR));
            const float dAmt = diffAmt.process();
            if (dAmt > 0.0001f)
            {
                sendL = diffL.process (sendL, dAmt);
                sendR = diffR.process (sendR, dAmt);
            }

            sendL *= (1.0f - fz);
            sendR *= (1.0f - fz);

            const float w = width.process();
            float writeL, writeR;
            if (mode == 0) // normal: width controls feedback crossfeed, not the dry path
            {
                const float cross = 0.5f + 0.5f * w;
                writeL = sendL + fbL * fb * cross + fbR * fb * (1.0f - cross);
                writeR = sendR + fbR * fb * cross + fbL * fb * (1.0f - cross);
            }
            else if (mode == 1) // ping-pong with variable crossfeed
            {
                const float mono = 0.5f * (sendL + sendR);
                const float cross = 0.5f + 0.5f * w;
                writeL = mono + fbR * fb * cross + fbL * fb * (1.0f - cross);
                writeR =       fbL * fb * cross + fbR * fb * (1.0f - cross);
            }
            else // classic ping-pong: input left, L -> R -> L
            {
                const float mono = 0.5f * (sendL + sendR);
                writeL = mono + fbR * fb;
                writeR = fbL * fb;
            }

            if (muteWrites)
            {
                writeL = 0.0f;
                writeR = 0.0f;
            }

            if (std::fabs (writeL) > 1.0f) writeL = softClip (writeL);
            if (std::fabs (writeR) > 1.0f) writeR = softClip (writeR);

            delayL.write (writeL);
            delayR.write (writeR);

            float gain = autoClearGain;
            const float duckAmt = duckAmtSmooth.process();
            if (duckAmt > 0.0001f)
                gain *= 1.0f - clamp (duck.process (dryL, dryR) * duckAmt, 0.0f, 1.0f);

            wetL *= gain;
            wetR *= gain;

            // output stereo width (mid/side)
            const float st = stereo.process();
            if (std::fabs (st - 1.0f) > 0.0001f)
            {
                const float mid = 0.5f * (wetL + wetR);
                const float side = 0.5f * (wetL - wetR) * st;
                wetL = mid + side;
                wetR = mid - side;
            }

            const float m = mix.process();
            const float dryGain = std::cos (m * kHalfPi);
            const float wetGain = std::sin (m * kHalfPi);
            const float og = output.process();

            float yL = dryL * dryGain + wetL * wetGain;
            float yR = dryR * dryGain + wetR * wetGain;
            yL = softLimit (flushDenormal (yL * og));
            yR = softLimit (flushDenormal (yR * og));

            outL[i] = yL;
            if (outR && outR != outL)
                outR[i] = yR;
        }
    }

private:
    // -----------------------------------------------------------------------
    // Factory programs
    // -----------------------------------------------------------------------
    std::vector<float> defaultValues() const
    {
        std::vector<float> v ((size_t) kNumParams);
        for (int i = 0; i < kNumParams; ++i)
            v[(size_t) i] = param (i).def;
        return v;
    }

    void program (const char* name, std::initializer_list<std::pair<int, float>> overrides)
    {
        auto v = defaultValues();
        for (const auto& o : overrides)
            v[(size_t) o.first] = o.second;
        addProgram (name, std::move (v));
    }

    void addPrograms()
    {
        program ("Init", {});

        program ("Dub Chamber", {
            { pDivL, (float) kDivisionQuarter },
            { pDivR, (float) kDivisionEighth },
            { pFeedback, 60.0f }, { pMix, 32.0f },
            { pMode, 1.0f }, { pWidth, 45.0f },
            { pLowCut, 80.0f }, { pHighCut, 8000.0f },
            { pDrive, 12.0f }, { pTone, 60.0f },
            { pDiffAmt, 22.0f }, { pDiffSize, 40.0f },
            { pModDepth, 6.0f }, { pModRate, 0.5f },
            { pTape, 10.0f }, { pEqMid, -1.5f },
            { pAutoClear, 1.0f }, { pClearFade, 40.0f }, { pClearThresh, -42.0f },
        });

        program ("Tubby's Tape", {
            { pDivL, (float) kDivisionQuarter },
            { pDivR, (float) kDivisionQuarter },
            { pFeedback, 64.0f }, { pMix, 30.0f },
            { pLowCut, 100.0f }, { pHighCut, 6500.0f },
            { pDrive, 20.0f }, { pTone, 65.0f }, { pTape, 22.0f },
            { pDiffAmt, 12.0f }, { pDiffSize, 30.0f },
            { pModDepth, 5.0f }, { pModRate, 0.35f },
            { pEqMid, -2.5f }, { pEqLow, 1.0f },
            { pAutoClear, 1.0f }, { pClearFade, 50.0f }, { pClearThresh, -42.0f },
        });

        program ("Thunder Dub", {
            { pDivL, 14.0f }, { pDivR, (float) kDivisionQuarter },
            { pFeedback, 68.0f }, { pMix, 28.0f },
            { pMode, 1.0f }, { pWidth, 70.0f },
            { pLowCut, 110.0f }, { pHighCut, 6000.0f },
            { pDrive, 14.0f }, { pTone, 55.0f },
            { pDiffAmt, 30.0f }, { pDiffSize, 55.0f },
            { pModDepth, 10.0f }, { pModRate, 0.3f }, { pTape, 8.0f },
            { pEqMid, -2.0f },
            { pAutoClear, 1.0f }, { pClearFade, 70.0f }, { pClearThresh, -45.0f },
            { pClearHold, 20.0f },
        });

        program ("Dub Siren", {
            { pDivL, 9.0f }, { pDivR, (float) kDivisionQuarter },
            { pFeedback, 58.0f }, { pMix, 35.0f },
            { pMode, 1.0f }, { pWidth, 30.0f },
            { pLowCut, 130.0f }, { pHighCut, 9000.0f },
            { pDrive, 24.0f }, { pTone, 70.0f },
            { pDiffAmt, 8.0f }, { pDiffSize, 20.0f },
            { pModDepth, 18.0f }, { pModRate, 2.0f }, { pTape, 6.0f },
            { pEqMid, 1.0f },
            { pAutoClear, 1.0f }, { pClearFade, 20.0f }, { pClearThresh, -42.0f },
        });

        program ("Melodica Echo", {
            { pDivL, 5.0f }, { pDivR, 4.0f },
            { pFeedback, 48.0f }, { pMix, 30.0f },
            { pLowCut, 70.0f }, { pHighCut, 10000.0f },
            { pDrive, 4.0f }, { pTone, 55.0f }, { pTape, 4.0f },
            { pDiffAmt, 12.0f }, { pDiffSize, 25.0f },
            { pModDepth, 3.0f }, { pModRate, 0.8f },
            { pEqMid, -1.0f },
            { pAutoClear, 1.0f }, { pClearFade, 30.0f }, { pClearThresh, -42.0f },
        });

        program ("Spring Dub", {
            { pDivL, (float) kDivisionEighth }, { pDivR, (float) kDivisionEighth },
            { pFeedback, 52.0f }, { pMix, 33.0f },
            { pLowCut, 90.0f }, { pHighCut, 8000.0f },
            { pDrive, 4.0f }, { pTone, 55.0f }, { pTape, 3.0f },
            { pDiffAmt, 42.0f }, { pDiffSize, 50.0f },
            { pModDepth, 4.0f }, { pModRate, 0.5f },
            { pEqMid, -1.5f },
            { pAutoClear, 1.0f }, { pClearFade, 45.0f }, { pClearThresh, -42.0f },
        });

        program ("Dub Plate", {
            { pDivL, (float) kDivisionQuarter }, { pDivR, 9.0f },
            { pFeedback, 62.0f }, { pMix, 30.0f },
            { pLowCut, 120.0f }, { pHighCut, 6000.0f },
            { pDrive, 28.0f }, { pTone, 60.0f }, { pTape, 25.0f },
            { pDiffAmt, 8.0f }, { pDiffSize, 15.0f },
            { pModDepth, 8.0f }, { pModRate, 0.4f },
            { pEqLow, -1.0f }, { pEqMid, -3.0f },
            { pAutoClear, 1.0f }, { pClearFade, 50.0f }, { pClearThresh, -42.0f },
        });

        program ("Auto Clear Throw", {
            { pDivL, (float) kDivisionQuarter }, { pDivR, (float) kDivisionQuarter },
            { pFeedback, 56.0f }, { pMix, 40.0f },
            { pLowCut, 70.0f }, { pHighCut, 9000.0f },
            { pDrive, 6.0f }, { pTone, 55.0f }, { pTape, 6.0f },
            { pDiffAmt, 12.0f }, { pDiffSize, 25.0f },
            { pModDepth, 4.0f }, { pModRate, 0.8f },
            { pAutoClear, 1.0f }, { pClearFade, 12.0f }, { pClearThresh, -40.0f },
            { pClearHold, 40.0f },
        });

        program ("Classic Pong", {
            { pDivL, 14.0f }, { pDivR, 9.0f },
            { pFeedback, 58.0f }, { pMix, 30.0f },
            { pMode, 2.0f }, { pWidth, 60.0f },
            { pLowCut, 100.0f }, { pHighCut, 8000.0f },
            { pDrive, 6.0f }, { pTone, 50.0f },
            { pAutoClear, 1.0f }, { pClearFade, 35.0f },
        });

        program ("Dub Clean", {
            { pDivL, (float) kDivisionQuarter }, { pDivR, (float) kDivisionQuarter },
            { pFeedback, 45.0f }, { pMix, 30.0f },
            { pLowCut, 50.0f }, { pHighCut, 12000.0f },
            { pDrive, 0.0f }, { pTone, 50.0f },
            { pDiffAmt, 8.0f }, { pDiffSize, 20.0f },
            { pModDepth, 3.0f }, { pModRate, 0.4f }, { pTape, 2.0f },
            { pAutoClear, 0.0f },
        });
    }

    DelayLine delayL, delayR;
    Diffuser diffL, diffR;

    Smoother timeL, timeR, feedback, mix, width, stereo, drive, output, modDepth, diffAmt;
    Smoother tapeSmooth, duckAmtSmooth, lowCutSmooth, highCutSmooth, toneSmooth;
    Smoother freezeSmooth, eqLowSmooth, eqMidSmooth, eqHighSmooth;
    Biquad lowCutL, lowCutR, highCutL, highCutR;
    Biquad sendLowL, sendLowR, sendHighL, sendHighR;
    OnePoleLP satToneL, satToneR;
    Sat2x satL, satR;
    Biquad eqLowL, eqLowR, eqMidL, eqMidR, eqHighL, eqHighR;
    DCBlocker dcL, dcR;

    AutoClear autoClear;
    DuckFollower duck;

    float maxDelaySamples = 176400.0f;
    float modPhase = 0.0f;
    float wowPhase = 0.0f;
    float flutterPhase = 0.0f;
};

MPC_PLUGIN_ENTRY (QDelayMpc)
