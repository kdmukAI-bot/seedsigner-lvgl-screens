// seed_transcribe_freeform_qr_screen
//
// PROTOTYPE — a new SeedQR hand-transcription mode with NO Python parent. It is a
// companion to seed_transcribe_zoomed_qr_screen (fixed zone-stepping) and
// seed_transcribe_whole_qr_screen (full overview), aimed squarely at the larger,
// brighter touch panels (480x320, 800x480). The 240x240 Pi Zero is too small for
// this workflow — the app gates it to large displays — but it renders everywhere so
// the corpus can show why.
//
// THE IDEA — the screen as a light table. Lay a printed SeedQR paper template (the
// blank grid with the finder patterns pre-printed) directly ON the display. The QR is
// rendered the CONVENTIONAL way — black modules on a bright white field — so it looks
// exactly like the printed code you are copying: black parts are black, no mental
// inversion. The white field is the backlight; it shines up through the thin paper while
// the black modules block it, so the cells to mark read as the dark cells, just like the
// print. Zoom until the on-screen module pitch matches your printed template's cell pitch,
// line the template's finder patterns up over the screen's, mark the dark cells, then DRAG
// the QR to bring the next block under the template and keep going until the seed is fully
// transcribed.
//
// Two things the earlier zoomed screen deliberately does NOT do, and this one does:
//   1. ADJUSTABLE ZOOM. pixels_per_module is a live value the user drives, so the
//      module pitch can be dialled to a physical paper template instead of a fixed
//      24*px_multiplier. Range = [whole-QR-fits .. a few modules fill the screen].
//   2. FREE PAN. Not zone-stepping — a continuous drag (touch) / D-pad nudge
//      (hardware) that positions any part of the QR anywhere on the glass.
//
// INPUT, per modality (chosen by the input profile / cfg override, like the sibling
// screen). The two paths are mutually exclusive — a build only wires one:
//   TOUCH (the large panels): a thin VERTICAL ZOOM SLIDER pinned to the LEFT EDGE
//     (mostly out of the way) — slide UP to zoom in, DOWN to zoom out — plus DRAG
//     anywhere else to move the QR (direct manipulation: the QR follows the finger).
//     A top-right gutter "X" exits.
//   HARDWARE (the joystick panels, e.g. a 240-tall build): the D-PAD pans, KEY1 zooms
//     in, KEY3 zooms out, and KEY2 / the joystick CLICK exit. A bottom hint line shows
//     the exit affordance (Python-style; hardware has no on-screen buttons).
//
// RENDERING. Same "oversized-without-an-oversized-buffer" trick as the zoomed screen:
// there is NO giant QR bitmap. The qrcodegen module matrix (bundled encoder, shared via
// qr_encode_bytes) is direct-drawn per module in a DRAW_MAIN_END callback, and only the
// modules whose cell intersects the viewport are painted, so a pan is just a change of
// draw offset and a zoom is just a change of module pitch — both stay within the ESP32
// LVGL pool no matter how far you zoom in. Conventional polarity: the field is white, "on"
// modules are solid black cells, and thin light-gray gridlines are drawn on top at every
// module boundary so the transcriber can register their paper grid to the screen grid.
//
// Chrome-free tier (no top-nav scaffold); stateful Tier-2 lifecycle (one heap ctx owning
// the keypad group + qrcodegen buffers, torn down in the LV_EVENT_DELETE cleanup).
//
// cfg:
//   qr_data          (string, required, non-empty)  the SeedQR payload — a digit stream
//            (standard SeedQR) or CompactSeedQR entropy bytes, carried per data_encoding.
//   qr_mode          (string, default "numeric")    qrcodegen segment mode:
//            numeric | alphanumeric | byte | auto. SeedQR is numeric; CompactSeedQR is byte.
//   data_encoding    (string, default "utf8")       JSON transport encoding of qr_data:
//            utf8 | hex | base64 (binary CompactSeedQR payloads arrive hex-encoded).
//   exit_text        (string, required)             localized exit hint (host-translated),
//            rendered only in hardware mode but required in both — one uniform contract.
//   initial_pixels_per_module (int, optional)       starting module pitch (zoom); clamped to
//            the computed [min..max]. Absent -> a moderate default. Used by the screenshot
//            generator to frame an interesting zoom.
//   initial_center_module_x   (int, optional)       the QR module centered on the screen at
//   initial_center_module_y   (int, optional)       load (default: the QR's center). Lets the
//            corpus frame a finder pattern instead of an anonymous field of data cells.
//   show_gridlines   (bool, default true)           draw the per-module registration grid.
//   input.mode       (string, optional)             "touch" | "hardware" override, else the
//            platform input profile decides.
//
// Note: Python's num_modules field (if present in a scenario) is deliberately NOT read —
// the module count comes from the encoded matrix (qrcodegen_getSize).

#include "screen_scaffold.h"  // parse_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"       // decl, SEEDSIGNER_RET_BACK_BUTTON, seedsigner_lvgl_on_button_selected, seedsigner_lvgl_is_static_render
#include "gui_constants.h"    // ACCENT_COLOR, BODY_FONT, BODY_FONT_COLOR, BACKGROUND_COLOR, COMPONENT_PADDING, active_profile
#include "input_profile.h"    // input_mode_t, INPUT_MODE_TOUCH/HARDWARE, input_profile_get_mode
#include "navigation.h"       // nav_aux_key_index (shared KEY1/2/3 recognizer)
#include "qr_core.h"          // qr_encode_mode_t, qr_decode_payload, qr_encode_bytes, build_gutter_close_button
#include "screen_helpers.h"   // nav_mode_override_from_cfg

#include "lvgl.h"             // widgets, direct-draw layer API, lv_slider, lv_group/lv_indev

#include <nlohmann/json.hpp>  // json (cfg reads)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (decoded QR payload bytes)

// Feature-gated last: LVGL's bundled qrcodegen supplies the module matrix. This file
// lives one level deeper (screens/), so the repo-root-relative reach needs an extra ../.
#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

using json = nlohmann::json;


#if LV_USE_QRCODE

namespace {

// ---------------------------------------------------------------------------
// Screen context
// ---------------------------------------------------------------------------

// Pure POD (pointers/ints/bools only), so it is allocated with lv_malloc + lv_memzero
// and released with lv_free in the LV_EVENT_DELETE cleanup (any C++ member would need
// new/delete for ctor/dtor correctness).
struct seed_transcribe_freeform_qr_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *qr_field;    // full-screen field; modules direct-drawn in the draw cb
    lv_obj_t   *slider;      // touch zoom slider (NULL in hardware mode)
    lv_group_t *group;       // hardware keypad group (NULL in touch mode)

    input_mode_t input_mode;
    bool         emitted;    // exit reported once

    // QR geometry.
    int size;                // qrcodegen module count per side
    int screen_width, screen_height;

    // Zoom: the live module pitch, bounded so the whole QR always fits at the low end
    // and a few modules fill the screen at the high end.
    int pixels_per_module;
    int min_ppm, max_ppm;

    // Pan: the QR's top-left corner, in field-local pixels (the field fills the screen,
    // so this is effectively the screen position). Negative when the QR overflows the
    // top/left edge. Module (mx,my) is painted at (pan + m*pixels_per_module).
    int pan_x, pan_y;

    // Touch drag bookkeeping (press anchor + the pan captured at press, so the drag is
    // absolute — pan = pan_at_press + (current - press) — with no accumulation drift).
    int press_x, press_y;
    int pan_at_press_x, pan_at_press_y;

    uint8_t *encode_scratch; // qrcodegen scratch buffer (one-shot encode at build time)
    uint8_t *qr_matrix;      // encoded qrcodegen module matrix
    bool     have_frame;

    bool show_gridlines;     // draw the per-module registration grid on top
};

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

// Round a double to the nearest int without pulling in <cmath> (matches the repo's
// minimal-include screens).
int seed_transcribe_freeform_qr_round(double value) {
    return (int)(value + (value >= 0.0 ? 0.5 : -0.5));
}

// Keep the QR from being dragged entirely off the glass: at least a small margin of it
// must remain on screen on each axis, so the user always has something to grab and can
// never "lose" the code. The QR spans [pan, pan + size*ppm]; require `margin` px of that
// span to stay inside [0, screen]. When the whole QR is smaller than the screen this
// still allows free movement within the interior.
void seed_transcribe_freeform_qr_clamp_pan(seed_transcribe_freeform_qr_ctx_t *ctx) {
    int qr_px = ctx->size * ctx->pixels_per_module;

    // Keep ~3 modules (or 40 px, whichever is larger) reachable, but never more than the
    // QR itself is wide.
    int margin_x = LV_MIN(qr_px, LV_MAX(3 * ctx->pixels_per_module, 40));
    int margin_y = LV_MIN(qr_px, LV_MAX(3 * ctx->pixels_per_module, 40));

    ctx->pan_x = LV_CLAMP(margin_x - qr_px, ctx->pan_x, ctx->screen_width  - margin_x);
    ctx->pan_y = LV_CLAMP(margin_y - qr_px, ctx->pan_y, ctx->screen_height - margin_y);
}

// Re-anchor the pan so the QR module currently under the screen CENTER stays under the
// center after a zoom change — the natural "zoom toward the middle of what I'm looking
// at" behaviour. Then clamp + repaint.
void seed_transcribe_freeform_qr_set_zoom(seed_transcribe_freeform_qr_ctx_t *ctx, int new_ppm) {
    new_ppm = LV_CLAMP(ctx->min_ppm, new_ppm, ctx->max_ppm);
    if (new_ppm == ctx->pixels_per_module) return;

    double center_x = ctx->screen_width  / 2.0;
    double center_y = ctx->screen_height / 2.0;
    // The (fractional) QR-module coordinate presently at the screen center.
    double module_cx = (center_x - ctx->pan_x) / (double)ctx->pixels_per_module;
    double module_cy = (center_y - ctx->pan_y) / (double)ctx->pixels_per_module;

    ctx->pixels_per_module = new_ppm;
    ctx->pan_x = seed_transcribe_freeform_qr_round(center_x - module_cx * new_ppm);
    ctx->pan_y = seed_transcribe_freeform_qr_round(center_y - module_cy * new_ppm);

    seed_transcribe_freeform_qr_clamp_pan(ctx);
    if (ctx->slider) lv_slider_set_value(ctx->slider, ctx->pixels_per_module, LV_ANIM_OFF);
    lv_obj_invalidate(ctx->qr_field);
}

// One zoom notch for the hardware KEY1/KEY3 path: ~1/8 of the usable range per press, at
// least 1, so ~8 presses span min..max.
int seed_transcribe_freeform_qr_zoom_step(seed_transcribe_freeform_qr_ctx_t *ctx) {
    return LV_MAX(1, (ctx->max_ppm - ctx->min_ppm) / 8);
}

// ---------------------------------------------------------------------------
// Direct-draw viewport
// ---------------------------------------------------------------------------

// Paint the visible "on" modules (as solid cells) plus the registration gridlines onto
// qr_field's layer. Only modules whose cell intersects the object area are drawn, so the
// pass is bounded by the viewport regardless of QR size or zoom.
void seed_transcribe_freeform_qr_draw_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx   = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t                        *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->have_frame) return;

    lv_area_t field_area;
    lv_obj_get_coords(ctx->qr_field, &field_area);  // absolute; the object fills the screen

    const int ppm  = ctx->pixels_per_module;
    const int size = ctx->size;

    // "On" modules are solid black cells on the white field — a conventional QR, so the
    // transcriber copies "black is black" with no mental inversion. The white field is the
    // backlight that shines up through the paper template; the black cells block it.
    lv_draw_rect_dsc_t module_dsc;
    lv_draw_rect_dsc_init(&module_dsc);
    module_dsc.bg_color = lv_color_black();
    module_dsc.bg_opa   = LV_OPA_COVER;

    for (int module_y = 0; module_y < size; module_y++) {
        int cell_y = field_area.y1 + ctx->pan_y + module_y * ppm;
        if (cell_y + ppm <= field_area.y1 || cell_y > field_area.y2) continue;  // vertical cull
        for (int module_x = 0; module_x < size; module_x++) {
            if (!qrcodegen_getModule(ctx->qr_matrix, module_x, module_y)) continue;
            int cell_x = field_area.x1 + ctx->pan_x + module_x * ppm;
            if (cell_x + ppm <= field_area.x1 || cell_x > field_area.x2) continue;  // horizontal cull
            // Full-cell squares so adjacent "on" modules tile into the connected shapes a
            // QR is made of (finder blocks, timing runs); the gridlines overlay them.
            lv_area_t module_area = { cell_x, cell_y, cell_x + ppm - 1, cell_y + ppm - 1 };
            lv_draw_rect(layer, &module_dsc, &module_area);
        }
    }

    if (!ctx->show_gridlines) return;

    // Dim gridlines at every module boundary (drawn ON TOP of the modules), spanning only
    // the QR content extent and clipped to the viewport — the per-cell grid the transcriber
    // registers their paper template against.
    lv_draw_rect_dsc_t grid;
    lv_draw_rect_dsc_init(&grid);
    // A light gray that reads against both the white field and the black cells.
    grid.bg_color = lv_color_hex(0xB0B0B0);
    grid.bg_opa   = LV_OPA_COVER;

    int content_top    = field_area.y1 + ctx->pan_y;
    int content_bottom = field_area.y1 + ctx->pan_y + size * ppm;
    int content_left   = field_area.x1 + ctx->pan_x;
    int content_right  = field_area.x1 + ctx->pan_x + size * ppm;
    int vertical_top     = LV_MAX(content_top, field_area.y1);
    int vertical_bottom  = LV_MIN(content_bottom, field_area.y2);
    int horizontal_left  = LV_MAX(content_left, field_area.x1);
    int horizontal_right = LV_MIN(content_right, field_area.x2);

    // i runs 0..size INCLUSIVE: size+1 boundaries close every cell, incl. the far-right
    // column edge and far-bottom row edge.
    for (int i = 0; i <= size; i++) {
        int gridline_x = field_area.x1 + ctx->pan_x + i * ppm;  // vertical line
        if (gridline_x >= field_area.x1 && gridline_x <= field_area.x2 &&
            vertical_top <= vertical_bottom) {
            lv_area_t line_area = { gridline_x, vertical_top, gridline_x, vertical_bottom };
            lv_draw_rect(layer, &grid, &line_area);
        }
        int gridline_y = field_area.y1 + ctx->pan_y + i * ppm;  // horizontal line
        if (gridline_y >= field_area.y1 && gridline_y <= field_area.y2 &&
            horizontal_left <= horizontal_right) {
            lv_area_t line_area = { horizontal_left, gridline_y, horizontal_right, gridline_y };
            lv_draw_rect(layer, &grid, &line_area);
        }
    }
}

// ---------------------------------------------------------------------------
// Exit + input callbacks
// ---------------------------------------------------------------------------

void seed_transcribe_freeform_qr_exit(seed_transcribe_freeform_qr_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "seed_transcribe_freeform_qr_done");
}

// Hardware keypad: D-pad pans (the arrow direction reveals that side of the QR — the
// viewport moves that way, so the content slides opposite), KEY1 zooms in, KEY3 zooms
// out, KEY2 / joystick CLICK exit.
void seed_transcribe_freeform_qr_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    uint32_t key = lv_event_get_key(e);

    // KEY1 zoom in / KEY2 exit / KEY3 zoom out (via the shared 1/2/3 recognizer).
    int aux = nav_aux_key_index(key);
    if (aux == 1) { seed_transcribe_freeform_qr_set_zoom(ctx, ctx->pixels_per_module + seed_transcribe_freeform_qr_zoom_step(ctx)); return; }
    if (aux == 3) { seed_transcribe_freeform_qr_set_zoom(ctx, ctx->pixels_per_module - seed_transcribe_freeform_qr_zoom_step(ctx)); return; }
    if (aux == 2) { seed_transcribe_freeform_qr_exit(ctx); return; }

    // One D-pad nudge ~= 2 modules (a controllable step that still moves meaningfully).
    int step = LV_MAX(2 * ctx->pixels_per_module, 24);
    switch (key) {
        case LV_KEY_RIGHT: ctx->pan_x -= step; break;  // reveal the QR's right side
        case LV_KEY_LEFT:  ctx->pan_x += step; break;
        case LV_KEY_DOWN:  ctx->pan_y -= step; break;
        case LV_KEY_UP:    ctx->pan_y += step; break;
        case LV_KEY_ENTER: seed_transcribe_freeform_qr_exit(ctx); return;  // joystick click exits
        default:           return;                                          // ignore unknown keys
    }
    seed_transcribe_freeform_qr_clamp_pan(ctx);
    lv_obj_invalidate(ctx->qr_field);
}

// Touch drag: direct manipulation — the QR follows the finger (pan = pan_at_press + drag
// delta). Captured absolutely from the press anchor so there is no per-move accumulation.
void seed_transcribe_freeform_qr_press_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    ctx->press_x = point.x;
    ctx->press_y = point.y;
    ctx->pan_at_press_x = ctx->pan_x;
    ctx->pan_at_press_y = ctx->pan_y;
}
void seed_transcribe_freeform_qr_pressing_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    ctx->pan_x = ctx->pan_at_press_x + (point.x - ctx->press_x);
    ctx->pan_y = ctx->pan_at_press_y + (point.y - ctx->press_y);
    seed_transcribe_freeform_qr_clamp_pan(ctx);
    lv_obj_invalidate(ctx->qr_field);
}

// Touch zoom slider: value IS the module pitch (bottom = min, top = max, so sliding up
// zooms in). Re-anchor on the center so the view stays put as the pitch changes.
void seed_transcribe_freeform_qr_slider_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->slider) return;
    seed_transcribe_freeform_qr_set_zoom(ctx, (int)lv_slider_get_value(ctx->slider));
}

// The "+"/"-" end-cap taps: MICRO zoom (±1 px/module) for dialling the module pitch to a
// paper template exactly, versus the slider's gross drag. set_zoom re-anchors + moves the
// slider knob to match, so the two controls stay in sync.
void seed_transcribe_freeform_qr_zoom_in_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (ctx) seed_transcribe_freeform_qr_set_zoom(ctx, ctx->pixels_per_module + 1);
}
void seed_transcribe_freeform_qr_zoom_out_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (ctx) seed_transcribe_freeform_qr_set_zoom(ctx, ctx->pixels_per_module - 1);
}

// A tappable "+"/"-" micro-zoom button: a small translucent rounded square with an accent
// glyph, flashing on press. Fires `cb` on tap (LV_EVENT_PRESSED) AND repeatedly while held
// (LV_EVENT_LONG_PRESSED_REPEAT) so a press-and-hold keeps trimming — no CLICKED, so the
// two never double-count one press.
lv_obj_t *seed_transcribe_freeform_qr_zoom_button(lv_obj_t *parent, const char *glyph,
                                                  int x, int y, int size,
                                                  lv_event_cb_t cb, void *user_data) {
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, size / 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    // Press flash: brighten the fill so the tap registers visibly (accent glyph stays legible).
    lv_obj_set_style_bg_color(button, lv_color_white(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, glyph);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_center(label);

    lv_obj_add_event_cb(button, cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(button, cb, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    return button;
}

void seed_transcribe_freeform_qr_close_cb(lv_event_t *e) {
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (ctx) seed_transcribe_freeform_qr_exit(ctx);
}

// LV_EVENT_DELETE teardown on the screen root: delete the hardware keypad group, free the
// qrcodegen buffers, free the ctx.
void seed_transcribe_freeform_qr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    seed_transcribe_freeform_qr_ctx_t *ctx = (seed_transcribe_freeform_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->group)          lv_group_del(ctx->group);
    if (ctx->encode_scratch) lv_free(ctx->encode_scratch);
    if (ctx->qr_matrix)      lv_free(ctx->qr_matrix);
    lv_free(ctx);
}

}  // namespace

#endif  // LV_USE_QRCODE


void seed_transcribe_freeform_qr_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a blank
    // screen so the entry point exists and navigation does not crash.
    (void)ctx_json;
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    load_screen_and_cleanup_previous(screen_root);
#else
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required-field validation: one throw per field, before any allocation, so no throw
    // path can leak the ctx or LVGL objects.
    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("seed_transcribe_freeform_qr_screen: qr_data is required and must be a non-empty string");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();

    std::string qr_mode_string = cfg.value("qr_mode", std::string("numeric"));
    qr_encode_mode_t qr_mode;
    if (qr_mode_string == "numeric")            qr_mode = QR_ENC_NUMERIC;
    else if (qr_mode_string == "alphanumeric")  qr_mode = QR_ENC_ALNUM;
    else if (qr_mode_string == "byte")          qr_mode = QR_ENC_BYTE;
    else if (qr_mode_string == "auto")          qr_mode = QR_ENC_AUTO;
    else throw std::runtime_error("seed_transcribe_freeform_qr_screen: qr_mode must be numeric|alphanumeric|byte|auto");

    std::string data_encoding = cfg.value("data_encoding", std::string("utf8"));
    if (data_encoding != "utf8" && data_encoding != "hex" && data_encoding != "base64")
        throw std::runtime_error("seed_transcribe_freeform_qr_screen: data_encoding must be utf8|hex|base64");

    // exit_text is user-visible CONTENT: it always arrives already localized from the host
    // (an English fallback baked here would ship untranslated). Required in both input modes
    // even though only hardware mode renders it (one uniform contract).
    if (!cfg.contains("exit_text") || !cfg["exit_text"].is_string()) {
        throw std::runtime_error("seed_transcribe_freeform_qr_screen: exit_text is required and must be a string");
    }
    std::string exit_text = cfg["exit_text"].get<std::string>();

    bool show_gridlines = cfg.value("show_gridlines", true);

    bool has_override = false;
    input_mode_t input_mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_override, input_mode_override);
    input_mode_t input_mode = has_override ? input_mode_override : input_profile_get_mode();

    // --- QR encode + zoom geometry ---

    std::vector<uint8_t> payload = qr_decode_payload(qr_data, data_encoding);

    seed_transcribe_freeform_qr_ctx_t *ctx =
        (seed_transcribe_freeform_qr_ctx_t *)lv_malloc(sizeof(seed_transcribe_freeform_qr_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->input_mode     = input_mode;
    ctx->show_gridlines = show_gridlines;
    ctx->encode_scratch = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    ctx->qr_matrix      = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);

    // Encode once (static QR; no host frame push). match_python_mask=true so the
    // hand-transcribed pattern is pixel-identical to a Pi Zero. On failure we still build
    // the screen so navigation doesn't crash; the field stays blank.
    ctx->have_frame = qr_encode_bytes(qr_mode, payload.data(), payload.size(),
                                      ctx->encode_scratch, ctx->qr_matrix,
                                      /*match_python_mask=*/true);
    ctx->size = ctx->have_frame ? qrcodegen_getSize(ctx->qr_matrix) : 21;

    ctx->screen_width  = lv_display_get_horizontal_resolution(NULL);
    ctx->screen_height = lv_display_get_vertical_resolution(NULL);

    // Zoom bounds:
    //   min = whole QR fits within the SHORTER screen edge (zoom-out-to-orient floor).
    //   max = ~a third of the shorter edge per module (a few big modules fill the glass —
    //         enough pitch to match a generously-printed paper template).
    int short_edge = LV_MIN(ctx->screen_width, ctx->screen_height);
    ctx->min_ppm = LV_MAX(4, short_edge / LV_MAX(ctx->size, 1));
    ctx->max_ppm = LV_MAX(ctx->min_ppm + 1, short_edge / 3);

    // Starting zoom: cfg override (clamped) else a moderate default (~a dozen modules down
    // the short edge) so the tracing detail is visible without hunting the slider.
    int default_ppm = LV_CLAMP(ctx->min_ppm, short_edge / 12, ctx->max_ppm);
    int initial_ppm = cfg.contains("initial_pixels_per_module")
                          ? cfg["initial_pixels_per_module"].get<int>() : default_ppm;
    ctx->pixels_per_module = LV_CLAMP(ctx->min_ppm, initial_ppm, ctx->max_ppm);

    // Starting pan: center the requested QR module (default the QR's center) under the
    // screen center, then clamp so it stays on the glass.
    int center_module_x = cfg.value("initial_center_module_x", ctx->size / 2);
    int center_module_y = cfg.value("initial_center_module_y", ctx->size / 2);
    center_module_x = LV_CLAMP(0, center_module_x, ctx->size - 1);
    center_module_y = LV_CLAMP(0, center_module_y, ctx->size - 1);
    ctx->pan_x = ctx->screen_width  / 2 - (center_module_x * ctx->pixels_per_module + ctx->pixels_per_module / 2);
    ctx->pan_y = ctx->screen_height / 2 - (center_module_y * ctx->pixels_per_module + ctx->pixels_per_module / 2);
    seed_transcribe_freeform_qr_clamp_pan(ctx);

    // --- Bare-root build ---

    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);
    ctx->screen = screen_root;

    // 1) The QR field: fills the screen; white (the QR's "off"/quiet-zone colour and the
    //    backlight that shines through the paper template). Modules are direct-drawn on top
    //    in the draw cb.
    ctx->qr_field = lv_obj_create(screen_root);
    lv_obj_remove_style_all(ctx->qr_field);
    lv_obj_set_size(ctx->qr_field, ctx->screen_width, ctx->screen_height);
    lv_obj_set_pos(ctx->qr_field, 0, 0);
    lv_obj_set_style_bg_color(ctx->qr_field, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->qr_field, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->qr_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ctx->qr_field, seed_transcribe_freeform_qr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);

    // 2) Input affordances — the two modes are mutually exclusive.
    if (input_mode == INPUT_MODE_HARDWARE) {
        // Bottom hint plate (Python-style; hardware has no on-screen buttons). The joystick
        // click / KEY2 exits; D-pad pans; KEY1/KEY3 zoom.
        lv_obj_t *exit_hint_label = lv_label_create(screen_root);
        lv_label_set_text(exit_hint_label, exit_text.c_str());
        lv_obj_set_style_text_font(exit_hint_label, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(exit_hint_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_color(exit_hint_label, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(exit_hint_label, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(exit_hint_label, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(exit_hint_label, COMPONENT_PADDING / 4, LV_PART_MAIN);
        lv_obj_align(exit_hint_label, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Keypad sink (1x1, off-screen input target) owning the arrows + KEY1/2/3 + click.
        lv_obj_t *sink = lv_obj_create(screen_root);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, seed_transcribe_freeform_qr_key_cb, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    } else {
        // Full-screen drag catcher (BEHIND the slider + gutter X, which are added after and
        // therefore sit on top in their strips). Pressing it drags the QR.
        lv_obj_t *catcher = lv_obj_create(screen_root);
        lv_obj_remove_style_all(catcher);
        lv_obj_set_size(catcher, ctx->screen_width, ctx->screen_height);
        lv_obj_set_pos(catcher, 0, 0);
        lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(catcher, seed_transcribe_freeform_qr_press_cb,    LV_EVENT_PRESSED,  ctx);
        lv_obj_add_event_cb(catcher, seed_transcribe_freeform_qr_pressing_cb, LV_EVENT_PRESSING, ctx);

        // Vertical zoom slider pinned to the LEFT EDGE, mostly out of the way. Bottom = zoom
        // out, top = zoom in, for GROSS adjustment. A translucent track lets the QR partly
        // show through so it obscures as little as possible. Tappable "+"/"-" MICRO-zoom
        // buttons cap each end (below).
        int edge     = EDGE_PADDING;
        int slider_w = LV_MAX(16, ctx->screen_width / 22);
        // "+"/"-" tap targets capping the slider ends — wider than the thin slider so they
        // are comfortable to tap. They are the widest element, so the column is inset by the
        // standard EDGE_PADDING on the left/top/bottom and the slider is centered under them.
        int button_size     = LV_MAX(34, slider_w + 12);
        int end_gap         = LV_MAX(4, COMPONENT_PADDING / 2);
        int column_center_x = edge + button_size / 2;
        int slider_y = edge + button_size + end_gap;
        int slider_h = LV_MAX(slider_w, ctx->screen_height - 2 * (edge + button_size + end_gap));

        lv_obj_t *slider = lv_slider_create(screen_root);
        lv_obj_set_size(slider, slider_w, slider_h);
        lv_obj_set_pos(slider, column_center_x - slider_w / 2, slider_y);
        lv_slider_set_range(slider, ctx->min_ppm, ctx->max_ppm);
        lv_slider_set_value(slider, ctx->pixels_per_module, LV_ANIM_OFF);
        // Track (translucent dark), filled indicator + knob in accent.
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_color(slider, lv_color_hex(0x808080), LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(ACCENT_COLOR), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(ACCENT_COLOR), LV_PART_KNOB);
        ctx->slider = slider;
        lv_obj_add_event_cb(slider, seed_transcribe_freeform_qr_slider_cb, LV_EVENT_VALUE_CHANGED, ctx);

        // "+" (top = zoom in) / "-" (bottom = zoom out) MICRO-zoom tap buttons capping the
        // slider ends, centered over the slider column: the slider is for gross adjustment,
        // these nudge ±1 px/module (tap or hold-to-repeat) so the scale dials to a paper
        // template exactly. Created AFTER the slider + catcher so they take the tap (z-order).
        seed_transcribe_freeform_qr_zoom_button(
            screen_root, "+", column_center_x - button_size / 2, edge, button_size,
            seed_transcribe_freeform_qr_zoom_in_cb, ctx);
        seed_transcribe_freeform_qr_zoom_button(
            screen_root, "-", column_center_x - button_size / 2,
            ctx->screen_height - edge - button_size, button_size,
            seed_transcribe_freeform_qr_zoom_out_cb, ctx);

        // Top-right gutter "X" -> exit (shared affordance; sits in the gutter, never over the QR).
        build_gutter_close_button(screen_root, seed_transcribe_freeform_qr_close_cb, ctx);
    }

    // --- Cleanup + load ---

    lv_obj_add_event_cb(screen_root, seed_transcribe_freeform_qr_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(screen_root);
#endif  // LV_USE_QRCODE
}
