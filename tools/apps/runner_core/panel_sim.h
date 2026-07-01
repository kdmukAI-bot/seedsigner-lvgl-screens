#ifndef SEEDSIGNER_PANEL_SIM_H
#define SEEDSIGNER_PANEL_SIM_H

#include <cstdint>

// panel_sim — an optional post-render intensity (gamma) curve that approximates the
// Pi Zero 240x240 ST7789 panel's steep brightness falloff.
//
// Why this exists: on the real ST7789 panel, mid-gray anti-aliased edge pixels are
// reproduced far dimmer (relative to the full-bright pixels) than they are on a laptop
// monitor, so thin/AA body text that looks fine in the desktop tools fades toward the
// background and becomes hard to read on-device. The framebuffer bytes are identical on
// both — only the display's transfer curve differs. This module simulates that curve so
// the readability problem can be reproduced and candidate fixes evaluated without hardware.
//
// It is a pure lookup table over an 8-bit channel value, applied in the display/output path
// only (the SDL blit and the screenshot RGB888 conversion) — it never touches the shared
// screen-rendering code, so it cannot affect layout or PIL pixel-parity.
//
// The transfer model is a single per-channel gamma: out = pow(v/255, gamma) * 255.
//   - gamma > 1 darkens mid-tones (e.g. 128 -> ~56 at gamma 2.2) while leaving 0 and 255
//     fixed — exactly the "AA edges fade into the background" effect.
//   - gamma == 1 (or disabled) is the identity curve.
// A single exponent is the documented starting point; a measured panel curve can replace
// the formula later without changing callers (they only consume lut()).
namespace panel_sim {

// Configure the simulation and rebuild the LUT. When `enabled` is false (or gamma == 1)
// the LUT is the identity, so callers can apply it unconditionally as a no-op.
void set_params(bool enabled, float gamma);

// Whether the simulation is currently active (false => identity LUT).
bool enabled();

// The current gamma exponent (meaningful only when enabled()).
float gamma();

// The active 256-entry lookup table, indexed by an 8-bit channel value. Always valid:
// identity until set_params() turns the simulation on.
const uint8_t* lut();

}  // namespace panel_sim

#endif  // SEEDSIGNER_PANEL_SIM_H
