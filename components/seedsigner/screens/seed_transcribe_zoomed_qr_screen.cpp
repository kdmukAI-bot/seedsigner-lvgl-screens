#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "qr_core.h"
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"
#include "locale_picker.h"
#include "overlay_manager.h"

#include "lvgl.h"

#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <set>
#include <map>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// seed_transcribe_zoomed_qr_screen — zoomed, pannable SeedQR transcription view
// (parity with Python SeedTranscribeSeedQRZoomedInScreen)
// ---------------------------------------------------------------------------
// A hand transcribes a SeedQR onto a paper template by reading it one "zone" (a
// 5x5 or 7x7 block of modules) at a time. This screen renders the QR OVERSIZED —
// each module is a fat 24 px (base) circle — and dims everything except the one
// centered zone, with an accent-outlined window plus A-F / 1-6 zone-coordinate
// labels so the reader always knows which template cell they are on. The joystick
// (or a touch swipe) steps one whole zone at a time; the joystick click (hardware) or
// a top-right X button (touch) exits.
//
// OVERSIZED WITHOUT AN OVERSIZED BUFFER. Python builds a giant square bitmap
// ((border+num_modules+border)*24 px per side = ~0.95-1.6 MB RGB565) and crops a
// display-sized window out of it each pan step, alpha-compositing a full-canvas
// zone mask on top. That buffer alone overflows the ESP32's fixed ~128 KB LVGL
// pool and would hard-freeze the device (the identical canvas-OOM class we fixed on
// qr_display_screen and psbt_overview_screen — see docs/qr-display-screen-esp32-
// canvas-oom.md). So there is NO oversized bitmap here: the module matrix (from the
// bundled qrcodegen, shared with qr_display_screen via qr_encode_bytes) is
// direct-drawn per-module in a DRAW_MAIN_END callback, and only the modules that
// fall inside the viewport are painted — panning is just a change of draw offset.
// The dimming mask, the accent window outline, and the zone-label bars are plain
// LVGL widgets (rectangles + labels), so LVGL recomposites them fringe-free with no
// per-frame memcpy.
//
// Geometry mirrors Python exactly at the 240 px reference (pixels_per_module = 24)
// and scales with the profile px_multiplier (~31 px at 320-height, 48 px at 480), so
// the zoom stays physically consistent across displays. Data modules are round dots;
// the fixed registration patterns (finder eyes + alignment block) are SOLID squares,
// matching both the qrcode lib's CircleModuleDrawer and — crucially — the pre-printed
// registration blocks on the paper SeedQR templates the transcriber fills in. Light
// #bbb gridlines overlay the whole content (incl. the solid blocks) as a counting
// guide. Rendering fidelity note: Python's rounded modules come from resizing a
// low-res StyledPilImage (soft, blurry circles); the direct-draw path paints crisp
// dots/squares instead — same visual language, sharper. The module matrix is
// pixel-identical for
// SeedQR (numeric); CompactSeedQR (byte) may pick a different mask than Python's
// qrcode lib, which is harmless (the transcribed QR still scans, and the confirm-
// scan step verifies it).

#if LV_USE_QRCODE

namespace {

// One zone-step of pan is animated over this long on live runners (snap on stills).
// Deliberately unhurried: the eye has to follow the sliding window to the next grid
// cell, so a slow ease-in-out slide reads far better than a quick snap.
constexpr uint32_t ZOOMQR_PAN_MS = 600;

// The 4-module quiet zone Python bakes around the oversized QR. It exists only so the
// viewport never runs off the matrix at an edge zone; we reproduce it as a draw offset
// (the white qr_obj background IS the quiet-zone field — no modules are drawn there).
constexpr int ZOOMQR_BORDER_MODULES = 4;

struct zoomqr_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *qr_obj;        // full-screen white field; modules direct-drawn in the draw cb
    lv_obj_t   *label_x;       // "1".."6" in the top accent bar (current column)
    lv_obj_t   *label_y;       // "A".."F" in the left accent bar (current row)
    lv_group_t *group;         // hardware keypad group (NULL in touch mode)

    input_mode_t input_mode;
    bool         emitted;      // exit reported once

    // QR + zone geometry (all in screen px unless noted).
    int size;                  // qrcodegen module count per side (== num_modules)
    int mpz;                   // modules_per_zone (7 for a 21-module QR, else 5)
    int ppm;                   // pixels_per_module (24 * px_multiplier / 100)
    int zones;                 // ceil(size / mpz): zone count per axis (square QR)
    int zone_px;               // mpz * ppm: the highlighted window edge
    int off_x, off_y;          // top-left of the centered zone window
    int W, H;

    // Current zone + the pan offset (top-left of the conceptual oversized crop).
    // Module (mx,my) is painted at screen (border+mx)*ppm - pan_x, (border+my)*ppm - pan_y.
    int cur_zone_x, cur_zone_y;
    int pan_x, pan_y;
    bool animating;            // ignore nav input mid-slide (Python's pan is blocking)
    lv_anim_t *pan_anim;

    // Touch swipe (press->release delta; LVGL's built-in gesture detection is velocity-
    // gated and didn't fire reliably here). A drag past swipe_threshold steps one zone.
    int press_x, press_y;
    int swipe_threshold;

    uint8_t *tmp;
    uint8_t *out;
    bool     have_frame;
};

// The pan offset that centers zone `z` in the window (Python's cur_pixel_x/y).
int zoomqr_pan_for_zone(zoomqr_ctx_t *ctx, int z, int off) {
    return z * ctx->mpz * ctx->ppm + ZOOMQR_BORDER_MODULES * ctx->ppm - off;
}

// Is module (mx,my) part of a fixed registration pattern (a finder "eye" or the
// alignment block)? Those are drawn as SOLID squares, not round dots — the SeedQR
// paper templates come with the registration blocks pre-printed in their normal
// square QR form, so the screen must match them (only the DATA modules are the fat
// dots the transcriber fills in). The qrcode lib's CircleModuleDrawer does the same:
// square eyes, round data. Alignment position follows Python's square-off (module 16
// for a 25-module QR, 20 for 29; version-1 21-module QRs have no alignment pattern).
bool zoomqr_is_registration(int mx, int my, int size) {
    bool finder = (mx < 7 && my < 7) ||                 // top-left
                  (mx >= size - 7 && my < 7) ||         // top-right
                  (mx < 7 && my >= size - 7);           // bottom-left
    int align = (size == 25) ? 16 : (size == 29) ? 20 : -1;
    bool alignment = align >= 0 &&
                     mx >= align && mx < align + 5 && my >= align && my < align + 5;
    return finder || alignment;
}

// Direct-draw the visible modules + the transcription gridlines onto qr_obj's layer
// (on top of its white background). Only modules whose cell intersects the object
// area are painted, so the pass is bounded by the viewport (~10x10 cells) no matter
// how large the QR is. Panning changes pan_x/pan_y and invalidates the object.
void zoomqr_draw_cb(lv_event_t *e) {
    zoomqr_ctx_t *ctx   = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t   *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->have_frame) return;

    lv_area_t obj;
    lv_obj_get_coords(ctx->qr_obj, &obj);  // absolute; the object fills the screen

    const int ppm    = ctx->ppm;
    const int border = ZOOMQR_BORDER_MODULES;
    const int size   = ctx->size;

    // Two black module styles: DATA modules are round dots; the fixed registration
    // patterns (finder eyes + alignment block) are SOLID squares so adjacent cells
    // tile into the connected shapes the paper templates pre-print (see
    // zoomqr_is_registration). Full-cell squares mean neighbours abut seamlessly.
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_black();
    dot.bg_opa   = LV_OPA_COVER;
    dot.radius   = LV_RADIUS_CIRCLE;
    lv_draw_rect_dsc_t square = dot;
    square.radius = 0;

    for (int my = 0; my < size; my++) {
        int sy = obj.y1 + (border + my) * ppm - ctx->pan_y;
        if (sy + ppm <= obj.y1 || sy > obj.y2) continue;         // vertical cull
        for (int mx = 0; mx < size; mx++) {
            if (!qrcodegen_getModule(ctx->out, mx, my)) continue;
            int sx = obj.x1 + (border + mx) * ppm - ctx->pan_x;
            if (sx + ppm <= obj.x1 || sx > obj.x2) continue;     // horizontal cull
            if (zoomqr_is_registration(mx, my, size)) {
                // Registration blocks fill the whole cell so neighbours tile into one
                // solid shape (the gridlines overlay them as a guide, by design).
                lv_area_t a = { sx, sy, sx + ppm - 1, sy + ppm - 1 };
                lv_draw_rect(layer, &square, &a);
            } else {
                // Data dots are inset by 1px so the 1px gridlines (drawn on top, at each
                // cell's top+left edge) FRAME the dot instead of clipping it — the whole
                // circle renders. Crisper + more accurate than Python's zoomed AA blur.
                lv_area_t a = { sx + 1, sy + 1, sx + ppm - 1, sy + ppm - 1 };
                lv_draw_rect(layer, &dot, &a);
            }
        }
    }

    // Light gridlines at every content-module boundary (Python draws them ON TOP of
    // the modules, in #bbb, spanning only the content area — never the quiet zone).
    // They give the transcriber a per-cell grid to count against.
    lv_draw_rect_dsc_t grid;
    lv_draw_rect_dsc_init(&grid);
    grid.bg_color = lv_color_hex(0xBBBBBB);
    grid.bg_opa   = LV_OPA_COVER;

    int content_top = obj.y1 + border * ppm - ctx->pan_y;
    int content_bot = obj.y1 + (border + size) * ppm - ctx->pan_y;
    int content_lft = obj.x1 + border * ppm - ctx->pan_x;
    int content_rgt = obj.x1 + (border + size) * ppm - ctx->pan_x;
    int vy1 = LV_MAX(content_top, obj.y1), vy2 = LV_MIN(content_bot, obj.y2);
    int hx1 = LV_MAX(content_lft, obj.x1), hx2 = LV_MIN(content_rgt, obj.x2);
    // i runs border..border+size INCLUSIVE: size+1 boundaries close every cell, incl. the
    // far-right column edge and far-bottom row edge (a bare `< border+size` left those open).
    for (int i = border; i <= border + size; i++) {
        int gx = obj.x1 + i * ppm - ctx->pan_x;                  // vertical line
        if (gx >= obj.x1 && gx <= obj.x2 && vy1 <= vy2) {
            lv_area_t a = { gx, vy1, gx, vy2 };
            lv_draw_rect(layer, &grid, &a);
        }
        int gy = obj.y1 + i * ppm - ctx->pan_y;                  // horizontal line
        if (gy >= obj.y1 && gy <= obj.y2 && hx1 <= hx2) {
            lv_area_t a = { hx1, gy, hx2, gy };
            lv_draw_rect(layer, &grid, &a);
        }
    }
}

// Rewrite the two zone-coordinate labels for the current zone ("3" / "C").
void zoomqr_update_labels(zoomqr_ctx_t *ctx) {
    char x[4]; lv_snprintf(x, sizeof(x), "%d", ctx->cur_zone_x + 1);
    char y[2] = { (char)('A' + ctx->cur_zone_y), 0 };
    lv_label_set_text(ctx->label_x, x);
    lv_label_set_text(ctx->label_y, y);
}

void zoomqr_pan_exec_x(void *var, int32_t v) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)var;
    ctx->pan_x = v;
    lv_obj_invalidate(ctx->qr_obj);
}
void zoomqr_pan_exec_y(void *var, int32_t v) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)var;
    ctx->pan_y = v;
    lv_obj_invalidate(ctx->qr_obj);
}
void zoomqr_pan_ready(lv_anim_t *a) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)a->var;
    ctx->animating = false;
    ctx->pan_anim  = NULL;
    // Reveal the destination coordinate only once the window has ARRIVED — updating the
    // A-F/1-6 labels mid-slide would show the target cell before the eye gets there.
    zoomqr_update_labels(ctx);
}

// Step by one zone in (dzx, dzy) if that stays on the QR, then slide the pan offset to
// the new zone (snapping on static stills). The zone-coordinate labels update on ARRIVAL
// (in zoomqr_pan_ready), not here, so they don't jump ahead of the window. Nav input is
// ignored while a slide is in flight, mirroring Python's blocking show_image_pan.
void zoomqr_step(zoomqr_ctx_t *ctx, int dzx, int dzy) {
    if (ctx->animating) return;
    int nx = ctx->cur_zone_x + dzx;
    int ny = ctx->cur_zone_y + dzy;
    if (nx < 0 || nx >= ctx->zones || ny < 0 || ny >= ctx->zones) return;
    if (nx == ctx->cur_zone_x && ny == ctx->cur_zone_y) return;

    ctx->cur_zone_x = nx;
    ctx->cur_zone_y = ny;

    int tx = zoomqr_pan_for_zone(ctx, nx, ctx->off_x);
    int ty = zoomqr_pan_for_zone(ctx, ny, ctx->off_y);

    if (seedsigner_lvgl_is_static_render()) {
        ctx->pan_x = tx;
        ctx->pan_y = ty;
        zoomqr_update_labels(ctx);   // no animation on stills: reflect the zone immediately
        lv_obj_invalidate(ctx->qr_obj);
        return;
    }

    // Only one axis changes per step; animate that one.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_time(&a, ZOOMQR_PAN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);  // gentle start+stop = easy to track
    lv_anim_set_ready_cb(&a, zoomqr_pan_ready);
    if (dzx) {
        lv_anim_set_values(&a, ctx->pan_x, tx);
        lv_anim_set_exec_cb(&a, zoomqr_pan_exec_x);
    } else {
        lv_anim_set_values(&a, ctx->pan_y, ty);
        lv_anim_set_exec_cb(&a, zoomqr_pan_exec_y);
    }
    ctx->animating = true;
    ctx->pan_anim  = lv_anim_start(&a);
}

void zoomqr_exit(zoomqr_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "seed_transcribe_zoomed_qr_done");
}

// Hardware joystick: arrows step one zone; any other key (a click) exits — Python's
// KEYS__LEFT_RIGHT_UP_DOWN pan / KEYS__ANYCLICK exit.
void zoomqr_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    switch (lv_event_get_key(e)) {
        case LV_KEY_RIGHT: zoomqr_step(ctx, +1,  0); break;
        case LV_KEY_LEFT:  zoomqr_step(ctx, -1,  0); break;
        case LV_KEY_DOWN:  zoomqr_step(ctx,  0, +1); break;
        case LV_KEY_UP:    zoomqr_step(ctx,  0, -1); break;
        default:           zoomqr_exit(ctx);         break;
    }
}

// Touch swipe = one full zone step, detected as a press->release delta (LVGL's built-in
// LV_EVENT_GESTURE is velocity-gated and did not fire reliably for a slow drag here, so we
// measure the drag ourselves). Content-drag / scroll sense: dragging the QR LEFT reveals
// the zone to its RIGHT, like panning a map. Full-step only — never free positioning. A
// drag shorter than swipe_threshold is a tap and does nothing (touch exits via the X).
void zoomqr_press_cb(lv_event_t *e) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ctx->press_x = p.x;
    ctx->press_y = p.y;
}
void zoomqr_release_cb(lv_event_t *e) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - ctx->press_x;
    int dy = p.y - ctx->press_y;
    int adx = LV_ABS(dx), ady = LV_ABS(dy);
    if (adx < ctx->swipe_threshold && ady < ctx->swipe_threshold) return;  // tap, not a swipe
    if (adx >= ady) {
        if (dx < 0) zoomqr_step(ctx, +1,  0);   // drag left  -> next column right
        else        zoomqr_step(ctx, -1,  0);
    } else {
        if (dy < 0) zoomqr_step(ctx,  0, +1);    // drag up    -> next row below
        else        zoomqr_step(ctx,  0, -1);
    }
}
void zoomqr_close_cb(lv_event_t *e) {
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    if (ctx) zoomqr_exit(ctx);
}

void zoomqr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->pan_anim) lv_anim_delete(ctx, NULL);
    if (ctx->group)    lv_group_del(ctx->group);
    if (ctx->tmp)      lv_free(ctx->tmp);
    if (ctx->out)      lv_free(ctx->out);
    lv_free(ctx);
}

// One flat, non-interactive dimming panel (black at Python's 226/255 opacity). The
// four of them frame the transparent zone window; kept out of the input path so taps
// and swipes reach the QR object beneath.
lv_obj_t *zoomqr_dim_rect(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, 226, LV_PART_MAIN);
    lv_obj_remove_flag(r, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return r;
}

// An accent zone-coordinate bar (top = column number, left = row letter) with a
// centered fixed-width label in the selected-font (dark) color, per Python.
lv_obj_t *zoomqr_bar(lv_obj_t *parent, int x, int y, int w, int h, lv_obj_t **out_label) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl, &KEYBOARD_FONT, LV_PART_MAIN);  // Inconsolata-SemiBold (fixed-width emphasis)
    lv_obj_set_style_text_color(lbl, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
    lv_obj_center(lbl);
    *out_label = lbl;
    return bar;
}

}  // namespace

#endif  // LV_USE_QRCODE

void seed_transcribe_zoomed_qr_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    (void)ctx_json;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    load_screen_and_cleanup_previous(scr);
#else
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("seed_transcribe_zoomed_qr_screen: non-empty qr_data (string) is required");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();

    // Encoding mode: SeedQR is numeric, CompactSeedQR is byte; "auto" mirrors qr_display.
    std::string mode_s = cfg.value("qr_mode", std::string("numeric"));
    qr_encode_mode_t mode;
    if (mode_s == "numeric")            mode = QR_ENC_NUMERIC;
    else if (mode_s == "alphanumeric")  mode = QR_ENC_ALNUM;
    else if (mode_s == "byte")          mode = QR_ENC_BYTE;
    else if (mode_s == "auto")          mode = QR_ENC_AUTO;
    else throw std::runtime_error("seed_transcribe_zoomed_qr_screen: qr_mode must be numeric|alphanumeric|byte|auto");

    std::string enc = cfg.value("data_encoding", std::string("utf8"));
    if (enc != "utf8" && enc != "hex" && enc != "base64")
        throw std::runtime_error("seed_transcribe_zoomed_qr_screen: data_encoding must be utf8|hex|base64");

    // Translated bottom hint (locale-agnostic screen; host passes it already translated).
    std::string exit_text = cfg.value("exit_text", std::string("click to exit"));

    bool has_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_override, mode_override);
    input_mode_t imode = has_override ? mode_override : input_profile_get_mode();

    std::vector<uint8_t> payload = qr_decode_payload(qr_data, enc);

    zoomqr_ctx_t *ctx = (zoomqr_ctx_t *)lv_malloc(sizeof(zoomqr_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->input_mode = imode;
    ctx->tmp = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    ctx->out = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);

    // Encode once (this is a static QR — no host frame push). match_python_mask=true so the
    // hand-transcribed pattern is pixel-identical to a Pi Zero (see qr_python_lost_point). On
    // failure we still build the screen so navigation doesn't crash; the field stays white.
    ctx->have_frame = qr_encode_bytes(mode, payload.data(), payload.size(), ctx->tmp, ctx->out,
                                      /*match_python_mask=*/true);
    ctx->size = ctx->have_frame ? qrcodegen_getSize(ctx->out) : 21;

    // Zone geometry. modules_per_zone follows Python: 7 for a 21-module QR (fills the
    // 240 screen nicely), 5 otherwise. pixels_per_module scales with the profile.
    ctx->mpz     = (ctx->size == 21) ? 7 : 5;
    // pixels_per_module = Python's fixed 24 at the 240 reference, scaled by the profile
    // multiplier (same truncating base*mult/100 the layout constants use).
    ctx->ppm     = (int)(24 * active_profile().px_multiplier / 100.0);
    ctx->zones   = (ctx->size + ctx->mpz - 1) / ctx->mpz;   // ceil
    ctx->zone_px = ctx->mpz * ctx->ppm;
    ctx->W       = lv_display_get_horizontal_resolution(NULL);
    ctx->H       = lv_display_get_vertical_resolution(NULL);
    ctx->off_x   = (ctx->W - ctx->zone_px) / 2;
    ctx->off_y   = (ctx->H - ctx->zone_px) / 2;

    // Initial zone (screenshot generator / host uses this to frame an interesting cell).
    int iz_x = cfg.value("initial_zone_x", 0);
    int iz_y = cfg.value("initial_zone_y", 0);
    ctx->cur_zone_x = LV_CLAMP(0, iz_x, ctx->zones - 1);
    ctx->cur_zone_y = LV_CLAMP(0, iz_y, ctx->zones - 1);
    ctx->pan_x = zoomqr_pan_for_zone(ctx, ctx->cur_zone_x, ctx->off_x);
    ctx->pan_y = zoomqr_pan_for_zone(ctx, ctx->cur_zone_y, ctx->off_y);

    // Full-bleed screen. The QR field is white (its quiet zone), black elsewhere is
    // never seen (the field fills the screen).
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    ctx->screen = scr;

    // 1) The QR field: white background, modules direct-drawn on top in the draw cb.
    ctx->qr_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(ctx->qr_obj);
    lv_obj_set_size(ctx->qr_obj, ctx->W, ctx->H);
    lv_obj_set_pos(ctx->qr_obj, 0, 0);
    lv_obj_set_style_bg_color(ctx->qr_obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->qr_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->qr_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ctx->qr_obj, zoomqr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);

    // 2) Dimming mask: four opaque-ish black panels framing the transparent zone
    //    window. The far panels are sized to the REMAINDER past the hole (not a second
    //    off_x/off_y) so an odd (W-zone_px)/(H-zone_px) can't truncate into a 1px gap
    //    that leaks the white QR field as a stray line (seen at 480x320).
    int hole_r = ctx->off_x + ctx->zone_px;   // right edge of the hole
    int hole_b = ctx->off_y + ctx->zone_px;   // bottom edge of the hole
    zoomqr_dim_rect(scr, 0, 0, ctx->W, ctx->off_y);                                  // top
    zoomqr_dim_rect(scr, 0, hole_b, ctx->W, ctx->H - hole_b);                        // bottom
    zoomqr_dim_rect(scr, 0, ctx->off_y, ctx->off_x, ctx->zone_px);                   // left
    zoomqr_dim_rect(scr, hole_r, ctx->off_y, ctx->W - hole_r, ctx->zone_px);         // right

    // 3) Accent window outline (1px) around the highlighted zone.
    lv_obj_t *outline = lv_obj_create(scr);
    lv_obj_remove_style_all(outline);
    lv_obj_set_pos(outline, ctx->off_x, ctx->off_y);
    lv_obj_set_size(outline, ctx->zone_px, ctx->zone_px);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(outline, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(outline, 1, LV_PART_MAIN);
    lv_obj_remove_flag(outline, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // 4) Zone-coordinate bars + labels: a column-number bar across the top and a
    //    row-letter bar down the left, both aligned to the zone window's extent.
    zoomqr_bar(scr, ctx->off_x, 0, ctx->zone_px, ctx->ppm, &ctx->label_x);   // top: "1".."6"
    zoomqr_bar(scr, 0, ctx->off_y, ctx->ppm, ctx->zone_px, &ctx->label_y);   // left: "A".."F"
    zoomqr_update_labels(ctx);

    // 5) Exit affordance + input. The two input modes get DIFFERENT exit affordances:
    //
    //    HARDWARE: a keypad sink (arrows pan a zone, any other key exits) + a bottom
    //    "click to exit" text plate — the joystick click is the exit, so the text tells
    //    the user what to press (Python parity; hardware has no on-screen button).
    //
    //    TOUCH: a transparent full-screen catcher (swipe = one-zone step; no tap-to-exit
    //    so a stray tap mid-transcription can't drop the user out) + an explicit top-right
    //    gutter "X" button. No bottom text — the X is the affordance.
    if (imode == INPUT_MODE_HARDWARE) {
        lv_obj_t *foot = lv_label_create(scr);
        lv_label_set_text(foot, exit_text.c_str());
        lv_obj_set_style_text_font(foot, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(foot, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_color(foot, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(foot, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(foot, COMPONENT_PADDING / 4, LV_PART_MAIN);
        lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);

        lv_obj_t *sink = lv_obj_create(scr);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, zoomqr_key_cb, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    } else {
        // Swipe distance that counts as a zone step: ~1/8 of the short screen edge,
        // floored so a small drag on a tiny display still needs deliberate movement.
        ctx->swipe_threshold = LV_MAX(30, LV_MIN(ctx->W, ctx->H) / 8);

        lv_obj_t *catcher = lv_obj_create(scr);
        lv_obj_remove_style_all(catcher);
        lv_obj_set_size(catcher, ctx->W, ctx->H);
        lv_obj_set_pos(catcher, 0, 0);
        lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);   // must be pressable to see the drag
        lv_obj_remove_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(catcher, zoomqr_press_cb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(catcher, zoomqr_release_cb, LV_EVENT_RELEASED, ctx);

        build_gutter_close_button(scr, zoomqr_close_cb, ctx);  // top-right X -> exit
    }

    lv_obj_add_event_cb(scr, zoomqr_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(scr);
#endif  // LV_USE_QRCODE
}
