// qr_core — the QR encode/decode core shared by the two chrome-free QR screens
// (qr_display_screen and seed_transcribe_zoomed_qr_screen).
//
// Both screens live in their own translation units under screens/, but they share
// one QR encoding/decoding core: the payload decoder (hex/base64/utf8 -> bytes) and
// the qrcodegen wrapper (which, for the hand-transcribed SeedQR screens, also
// reproduces python-qrcode's mask-pattern selection so the pattern the user copies
// matches a Pi Zero module-for-module). That core has internal linkage in neither
// screen — it is compiled once here, with external linkage, and both screens call it
// across the TU boundary via this header.
//
// Also hosts build_gutter_close_button(), the top-right "X" exit affordance shared by
// both chrome-free QR screens (placed in the screen gutter so it never overlaps the QR).
//
// NB: seed_transcribe_whole_qr_screen.cpp keeps its OWN private copies of the mask-parity
// helpers (in its anonymous namespace) so the fast qr_display path stays self-contained;
// those do not collide with the external-linkage definitions here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"

// How a payload is encoded into the QR. SeedQR stays NUMERIC; all-uppercase payloads
// (BBQR) use ALPHANUMERIC; binary (CompactSeedQR) is BYTE; AUTO picks the most compact
// mode the payload allows (mirroring Python's qrcode auto-detect).
enum qr_encode_mode_t { QR_ENC_NUMERIC, QR_ENC_ALNUM, QR_ENC_BYTE, QR_ENC_AUTO };

// Decode the cfg["qr_data"] string (which crossed the JSON boundary) per data_encoding
// ("utf8" | "hex" | "base64"). JSON can't carry raw bytes, so binary payloads arrive
// hex/base64-encoded. Throws std::runtime_error on malformed hex.
std::vector<uint8_t> qr_decode_payload(const std::string &s, const std::string &enc);

// Encode `data` (len bytes) into `out`, using `tmp` as qrcodegen scratch, per `mode`.
// Both `tmp` and `out` must be qrcodegen_BUFFER_LEN_MAX. Returns false if it can't fit.
// match_python_mask: false = qrcodegen's fast built-in auto mask (machine-scanned QRs);
// true = pick the same mask python-qrcode would (the hand-transcribed SeedQR screens).
bool qr_encode_bytes(qr_encode_mode_t mode, const uint8_t *data, size_t len,
                     uint8_t *tmp, uint8_t *out, bool match_python_mask);

// Top-right gutter "X" close button — the touch exit affordance for chrome-free QR
// screens. Placed in the screen GUTTER (aligned to the parent, not over the QR) so it
// can't overlap the top-right finder pattern and break scannability. `cb` fires on
// LV_EVENT_CLICKED with `user_data`.
lv_obj_t *build_gutter_close_button(lv_obj_t *parent, lv_event_cb_t cb, void *user_data);
