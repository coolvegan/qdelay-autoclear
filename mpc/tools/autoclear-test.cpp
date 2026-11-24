// Unit test for the Auto Clear behaviour strategies (no plugin host needed).
//
// Build:  g++ -std=c++17 -O2 -I src -o build/autoclear-test mpc/tools/autoclear-test.cpp
// Run:    build/autoclear-test
#include "dsp/AutoClearStrategies.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace qdelay;

namespace
{
int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}
}

int main()
{
    const float sr = 48000.0f;
    const int hitPeriod = 12000;      // 0.25 s
    const int frames = (int) (sr * 4.0f);

    ClearParams p;
    p.enabled = true;
    p.fadeMs = 40.0f;
    p.thresholdDb = -42.0f;
    p.holdMs = 20.0f;
    p.amount = 1.0f;

    for (int mode = 0; mode < 2; ++mode)          // 0 Transient, 1 Ducker
    {
        AutoClearEngine engine;
        engine.prepare (sr);
        engine.set (p, mode == 1, false);

        float prev = 1.0f;
        float maxStep = 0.0f;
        int wipes = 0;
        int hits = 0;
        bool inRange = true;
        float wet = 0.0f;

        for (int i = 0; i < frames; ++i)
        {
            const bool hit = (i % hitPeriod) < 400;
            if (hit && (i % hitPeriod) == 0)
            {
                ++hits;
                if (wet < 0.3f)
                    wet = 0.3f;
            }
            wet *= 0.99995f;

            const float peak = hit ? 0.8f : 0.0f;
            engine.observeWet (wet);

            bool clearNow = false;
            const float gain = engine.process (peak, clearNow);
            if (clearNow)
                ++wipes;

            if (gain < -0.001f || gain > 1.001f)
                inRange = false;
            maxStep = std::max (maxStep, std::fabs (gain - prev));
            prev = gain;

            // Switch mode at 2 s and check that the crossfade does not jump.
            if (i == (int) (sr * 2.0f))
            {
                engine.set (p, mode == 0, false);
                const float g2 = engine.process (peak, clearNow);
                maxStep = std::max (maxStep, std::fabs (g2 - prev));
                prev = g2;
            }
        }

        char label[128];
        std::snprintf (label, sizeof label, "%s: gain stays in 0..1", mode ? "Ducker" : "Transient");
        check (inRange, label);
        std::snprintf (label, sizeof label, "%s: no gain step above 0.10 (max %.4f)", mode ? "Ducker" : "Transient", maxStep);
        check (maxStep < 0.10f, label);
        std::snprintf (label, sizeof label, "%s: at most one wipe per hit (%d hits, %d wipes)", mode ? "Ducker" : "Transient", hits, wipes);
        check (wipes <= hits, label);
    }

    // Feedback scaling: off means exactly unity, on means it follows the gain.
    {
        AutoClearEngine engine;
        engine.prepare (sr);
        engine.set (p, true, false);
        bool clearNow = false;
        for (int i = 0; i < 1000; ++i)
            engine.process (0.0f, clearNow);
        check (std::fabs (engine.feedbackScale() - 1.0f) < 1e-4f, "feedback scale is unity when Clear in Feedback is off");

        engine.set (p, true, true);
        for (int i = 0; i < 2000; ++i)
        {
            engine.observeWet (0.3f);
            engine.process (0.8f, clearNow);
        }
        check (engine.feedbackScale() < 0.5f, "feedback scale follows the duck when enabled");
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "autoclear-test: PASS" : "autoclear-test: FAIL", failures);
    return failures == 0 ? 0 : 1;
}
