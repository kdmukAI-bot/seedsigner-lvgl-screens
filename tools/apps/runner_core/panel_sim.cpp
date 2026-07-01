#include "panel_sim.h"

#include <cmath>

namespace panel_sim {

namespace {

// Simulation state. Default: disabled (the LUT is the identity, so every display path
// can index it unconditionally with no visible effect until the user turns it on).
bool    g_enabled = false;
float   g_gamma   = 1.0f;
uint8_t g_lut[256];
bool    g_built   = false;  // false until rebuild() has filled g_lut at least once

// Recompute g_lut from the current state. An identity curve (disabled, or gamma == 1)
// short-circuits the pow() so "off" is exactly a 1:1 passthrough.
void rebuild() {
    const bool identity = (!g_enabled) || (g_gamma == 1.0f);

    for (int i = 0; i < 256; ++i) {
        if (identity) {
            g_lut[i] = static_cast<uint8_t>(i);
            continue;
        }

        // out = pow(v/255, gamma) * 255, rounded and clamped to [0, 255].
        float v   = static_cast<float>(i) / 255.0f;
        float out = std::pow(v, g_gamma) * 255.0f + 0.5f;

        if (out < 0.0f)   out = 0.0f;
        if (out > 255.0f) out = 255.0f;

        g_lut[i] = static_cast<uint8_t>(out);
    }

    g_built = true;
}

}  // namespace

void set_params(bool enabled, float gamma) {
    g_enabled = enabled;
    g_gamma   = gamma;
    rebuild();
}

bool enabled() { return g_enabled; }

float gamma() { return g_gamma; }

const uint8_t* lut() {
    // Guarantee a valid (identity) table even if set_params() was never called.
    if (!g_built) rebuild();
    return g_lut;
}

}  // namespace panel_sim
