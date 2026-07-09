#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
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
// screensaver_screen
// ---------------------------------------------------------------------------

extern "C" void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

typedef struct {
    lv_obj_t   *screen;
    lv_obj_t   *logo_img;
    lv_timer_t *timer;
    lv_group_t *group;
    float       center_x;  // logo center, float for sub-pixel accuracy
    float       center_y;
    float       vel_x;     // pixels per millisecond
    float       vel_y;
    uint32_t    last_tick;
    int32_t     screen_w;
    int32_t     screen_h;
    int32_t     logo_w;    // displayed width after zoom
    int32_t     logo_h;    // displayed height after zoom
    bool        route_dismiss;  // true: input fires a host dismiss result (legacy
                                // path); false: the overlay manager's idle-watch
                                // dismisses, so input is not host-routed here.
} screensaver_ctx_t;

// Speed range: 0.07 – 0.18 pixels/ms  (70 – 180 px/s).
static constexpr float SAVER_SPEED_MIN = 0.07f;
static constexpr float SAVER_SPEED_MAX = 0.18f;

// Minimum angle from the wall surface on departure (degrees).
// Prevents the logo from hugging a wall at a shallow grazing angle.
static constexpr float SAVER_MIN_WALL_ANGLE_RAD = 25.0f * 3.14159265f / 180.0f;

// Returns a random float in [lo, hi).
static float saver_randf(float lo, float hi) {
    uint32_t r = lv_rand(0, 0x7fffffffu);
    return lo + (hi - lo) * ((float)r / (float)0x7fffffffu);
}

// Pick a random departure angle within the half-plane defined by 'normal_angle'
// (the inward wall normal), clamped so the angle is at least SAVER_MIN_WALL_ANGLE
// away from either wall surface edge.  This eliminates wall-hugging trajectories.
static float saver_bounce_angle(float normal_angle) {
    float max_offset = (3.14159265f / 2.0f) - SAVER_MIN_WALL_ANGLE_RAD;
    float offset = saver_randf(-max_offset, max_offset);
    return normal_angle + offset;
}

static void screensaver_timer_cb(lv_timer_t *timer) {
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_timer_get_user_data(timer);

    // Legacy (Python-driven) path only: poll pointer devices and route a dismiss
    // to the host on touch. In overlay-manager mode (route_dismiss == false) the
    // manager's idle-watch handles dismissal — any input resets
    // lv_display_get_inactive_time() — so the screensaver does not route input.
    if (ctx->route_dismiss) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
                return;
            }
        }
    }

    uint32_t now     = lv_tick_get();
    uint32_t elapsed = now - ctx->last_tick;
    ctx->last_tick   = now;

    // Clamp elapsed to avoid huge jumps after screen switches or pauses.
    if (elapsed > 200) elapsed = 200;

    ctx->center_x += ctx->vel_x * (float)elapsed;
    ctx->center_y += ctx->vel_y * (float)elapsed;

    bool bounced_x = false;
    bool bounced_y = false;
    bool hit_left  = false;
    bool hit_top   = false;

    if (ctx->center_x < 0.0f) {
        ctx->center_x = 0.0f;
        bounced_x = true; hit_left = true;
    } else if (ctx->center_x > (float)ctx->screen_w) {
        ctx->center_x = (float)ctx->screen_w;
        bounced_x = true;
    }

    if (ctx->center_y < 0.0f) {
        ctx->center_y = 0.0f;
        bounced_y = true; hit_top = true;
    } else if (ctx->center_y > (float)ctx->screen_h) {
        ctx->center_y = (float)ctx->screen_h;
        bounced_y = true;
    }

    if (bounced_x || bounced_y) {
        // Inward normal angle for the wall(s) hit.
        // Screen coords: +x = right, +y = down.
        // Left wall  normal: 0          Right wall normal: π
        // Top wall   normal: π/2 (down) Bottom wall normal: -π/2 (up)
        float normal;
        if (bounced_x && bounced_y) {
            // Corner: diagonal normal pointing toward screen interior.
            normal = hit_left
                ? (hit_top ? (3.14159265f / 4.0f)        // top-left  → SE
                           : (-3.14159265f / 4.0f))       // bot-left  → NE
                : (hit_top ? (3.0f * 3.14159265f / 4.0f) // top-right → SW
                           : (-3.0f * 3.14159265f / 4.0f)); // bot-right → NW
        } else if (bounced_x) {
            normal = hit_left ? 0.0f : 3.14159265f;
        } else {
            normal = hit_top ? (3.14159265f / 2.0f) : (-3.14159265f / 2.0f);
        }

        float speed     = saver_randf(SAVER_SPEED_MIN, SAVER_SPEED_MAX);
        float new_angle = saver_bounce_angle(normal);
        ctx->vel_x = speed * cosf(new_angle);
        ctx->vel_y = speed * sinf(new_angle);
    }

    lv_obj_set_pos(ctx->logo_img,
                   (int32_t)(ctx->center_x - ctx->logo_w / 2.0f),
                   (int32_t)(ctx->center_y - ctx->logo_h / 2.0f));
}

static void screensaver_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
}

static void screensaver_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->timer) {
        lv_timer_del(ctx->timer);
        ctx->timer = NULL;
    }
    if (ctx->group) {
        lv_group_del(ctx->group);
        ctx->group = NULL;
    }
    lv_free(ctx);
}

// Build the screensaver screen (bouncing logo) WITHOUT loading it — the caller
// loads it. `route_dismiss_to_host` selects how the screensaver gets dismissed:
//   true  — legacy Python-driven path: a key/touch fires
//           SEEDSIGNER_RET_SCREENSAVER_DISMISS (via seedsigner_lvgl_on_button_selected)
//           and the host runner restores the saved screen.
//   false — overlay-manager path: the manager's idle-watch dismisses on any
//           input (lv_display_get_inactive_time() resets), so input is NOT
//           host-routed here. The keypad sink + group are still installed so the
//           wake keypress is swallowed rather than actuating the restored screen.
static lv_obj_t *ss_build_screensaver_impl(bool route_dismiss_to_host) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Display the logo at native resolution (no zoom). The image is pre-scaled
    // by png_to_lvgl.py and selected per display profile (px_multiplier) by
    // seedsigner_logo_for_active_profile().
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    int32_t logo_w = (int32_t)logo->header.w;
    int32_t logo_h = (int32_t)logo->header.h;

    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_set_size(logo_img, logo_w, logo_h);

    // Allocate and initialise animation context.
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_malloc(sizeof(screensaver_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen   = scr;
    ctx->logo_img = logo_img;
    ctx->screen_w = screen_w;
    ctx->screen_h = screen_h;
    ctx->logo_w   = logo_w;
    ctx->logo_h   = logo_h;
    ctx->route_dismiss = route_dismiss_to_host;

    // Start at screen center.
    ctx->center_x = screen_w / 2.0f;
    ctx->center_y = screen_h / 2.0f;

    // Random initial direction and speed.
    float init_speed = saver_randf(SAVER_SPEED_MIN, SAVER_SPEED_MAX);
    float init_angle = saver_randf(0.0f, 2.0f * 3.14159265f);
    ctx->vel_x = init_speed * cosf(init_angle);
    ctx->vel_y = init_speed * sinf(init_angle);

    ctx->last_tick = lv_tick_get();

    // Place logo at starting position.
    lv_obj_set_pos(logo_img,
                   (int32_t)(ctx->center_x - logo_w / 2.0f),
                   (int32_t)(ctx->center_y - logo_h / 2.0f));

    ctx->timer = lv_timer_create(screensaver_timer_cb, 16, ctx);

    // Keypad sink: any key press dismisses the screensaver.
    if (input_profile_get_mode() == INPUT_MODE_HARDWARE) {
        lv_obj_t *sink = lv_obj_create(scr);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        // Only the legacy path fires a host dismiss on keypress. In manager mode
        // the sink still swallows the wake keypress (no handler installed) while
        // the idle-watch performs the dismiss.
        if (route_dismiss_to_host) {
            lv_obj_add_event_cb(sink, screensaver_key_handler, LV_EVENT_KEY, ctx);
        }

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(scr, screensaver_cleanup_handler, LV_EVENT_DELETE, ctx);
    return scr;
}

// Internal shared builder (declared in seedsigner.h) — used by the overlay
// manager to build a manager-dismissed screensaver.
extern "C" lv_obj_t *ss_build_screensaver_obj(bool route_dismiss_to_host) {
    return ss_build_screensaver_impl(route_dismiss_to_host);
}

void screensaver_screen(void * /*ctx_json*/) {
    // Legacy entry point: build with host-routed dismiss and load WITHOUT
    // destroying the previous screen (the caller save/restores via
    // save_screen/restore_screen).
    lv_scr_load(ss_build_screensaver_impl(true));
}
