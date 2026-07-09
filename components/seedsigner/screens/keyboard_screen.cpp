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
// keyboard_screen
// ---------------------------------------------------------------------------
//
// Generalized single-charset text/char entry (the LVGL port of Python's
// KeyboardScreen). Unlike the passphrase screen there is one static page with no
// mode switching, so this is a plain lv_buttonmatrix (built on keyboard_core for
// the text-entry box, key theming, and joystick nav). Consumers: dice-roll /
// coin-flip entropy, BIP-85 child index, custom derivation path.
//
// Because the native screen owns the input loop and only returns the final
// string, two things that live in Python's KeyboardScreen move native here: the
// per-keystroke title (via `title_keystroke_template`) and `return_after_n_chars`
// auto-completion.
//
// cfg:
//   top_nav: { title, show_back_button, show_power_button }
//   cols (int, default 10)         grid columns; rows derive from the key count
//   keys (array of strings)        the value keys, one label per cell, row-major
//   keys_to_values (object)        optional label->emitted-value map (e.g. a dice
//                                  glyph -> "1"); absent => the label IS the value
//   keyboard_font ("fixed"|"icon") key glyph font (default "fixed" = KEYBOARD_FONT;
//                                  "icon"/"fontawesome" = ICON_FONT__SEEDSIGNER)
//   return_after_n_chars (int)     auto-return once this many chars are entered
//   show_save_button (bool)        append an in-grid green CHECK confirm key
//   initial_value (string)         prefill the text entry
//   title_keystroke_template (str) e.g. "Dice Roll {n}/{total}"; {n}=next entry
//                                  index, {total}=return_after_n_chars; live-updated
//   max_length (int)               optional cap on entered length
//
// A DELETE (backspace) key is always appended; CHECK is appended when
// show_save_button. Completion routes through seedsigner_lvgl_on_text_entered(),
// the same host hook the passphrase screen uses.

// Button-matrix control entries for keyboard_screen. KSW(n): a plain value key of
// relative width n. KSC(n): a control key (DELETE/CHECK) — marked CHECKED so
// kb_style_matrix paints it as a control + the icon draw-cb recolors it, plus
// NO_REPEAT/CLICK_TRIG so a hold doesn't auto-repeat.
#define KSW(n) ((lv_buttonmatrix_ctrl_t)(n))
#define KSC(n) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | (n)))

// Per-screen state. C++ (vectors/strings/map), so new/delete rather than
// lv_malloc; freed in keyboard_cleanup_cb. The text-entry box (c->ta) is the
// source of truth for the entered string — control keys edit it at the cursor.
struct keyboard_screen_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *ta = nullptr;
    lv_obj_t   *back_btn = nullptr;
    lv_obj_t   *top_nav = nullptr;       // for re-laying-out the title on each update
    lv_obj_t   *title_label = nullptr;
    bool        title_has_power = false; // power button present (for title centering)
    lv_group_t *group = nullptr;

    std::vector<std::string>           key_storage;  // backs the value-key char*s
    std::vector<const char *>          map;          // buttonmatrix map (persistent)
    std::vector<lv_buttonmatrix_ctrl_t> ctrl;        // one entry per button
    std::map<std::string, std::string> values;       // label -> emitted value

    std::string title_template;
    int  return_after = 0;   // 0 = no auto-return
    int  total = 0;          // {total} in the title template
};

// Substitute {n} / {total} in a title template.
static std::string keyboard_render_title(const std::string &tmpl, int n, int total) {
    std::string out = tmpl;
    auto rep = [&](const char *key, int v) {
        std::string s = std::to_string(v);
        size_t p;
        while ((p = out.find(key)) != std::string::npos) out.replace(p, std::strlen(key), s);
    };
    rep("{n}", n);
    rep("{total}", total);
    return out;
}

// Refresh the top-nav title from the template: {n} is the index of the entry the
// user is about to make (chars entered + 1, clamped to total).
static void keyboard_update_title(keyboard_screen_ctx_t *c) {
    if (c->title_template.empty() || !c->title_label || !lv_obj_is_valid(c->title_label)) return;
    int len = (c->ta && lv_obj_is_valid(c->ta)) ? (int)std::strlen(lv_textarea_get_text(c->ta)) : 0;
    int n = len + 1;
    if (c->total > 0 && n > c->total) n = c->total;
    lv_label_set_text(c->title_label,
                      keyboard_render_title(c->title_template, n, c->total).c_str());
    // The counter changes width as it grows, so re-run the top-nav title staging
    // (center → left-pin → scroll) against the new text instead of marquee-scrolling
    // within the slice top_nav measured for the initial value.
    top_nav_layout_title(c->top_nav, c->title_label, c->back_btn != nullptr,
                         c->title_has_power, nullptr);
}

static void keyboard_complete(keyboard_screen_ctx_t *c) {
    if (c->ta && lv_obj_is_valid(c->ta)) {
        seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->ta));
    }
}

// Buttonmatrix click handler (both input modes). The control glyphs act on the
// text-entry box directly so the cursor position is honored; any other key inserts
// its (possibly mapped) value at the cursor.
static void keyboard_value_changed_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target_obj(e);
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_obj_t *ta = c->ta;
    if (!ta || !lv_obj_is_valid(ta)) return;

    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(m, id);
    if (!txt) return;

    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0)        { lv_textarea_delete_char(ta);  keyboard_update_title(c); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(ta);  return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHECK) == 0) {
        if (std::strlen(lv_textarea_get_text(ta)) > 0) keyboard_complete(c);  // don't submit empty (Python parity)
        return;
    }

    // A value key: insert its mapped value (or the label) at the cursor. The
    // textarea enforces max_length natively.
    auto it = c->values.find(txt);
    lv_textarea_add_text(ta, (it != c->values.end()) ? it->second.c_str() : txt);
    keyboard_update_title(c);

    if (c->return_after > 0 && (int)std::strlen(lv_textarea_get_text(ta)) >= c->return_after) {
        keyboard_complete(c);
    }
}

// Hardware key filter: the generic top-nav handoff + row-wrap (no aux keys — the
// confirm/backspace are in-grid and joystick-navigable).
static void keyboard_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    kb_handle_directional(e, c->map.data(), c->matrix, c->back_btn);
}

static void keyboard_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->matrix);
}

static void keyboard_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) lv_group_del(c->group);
    delete c;
}

void keyboard_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Input mode selects the joystick group wiring (touch needs none).
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // No button_list: the body is a custom textarea + keyboard. upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    keyboard_screen_ctx_t *c = new keyboard_screen_ctx_t();
    c->back_btn = screen.top_back_btn;
    c->top_nav = screen.top_nav;
    c->title_label = screen.title_label;
    c->title_has_power = (screen.top_power_btn != nullptr);

    int cols = 10;
    if (cfg.contains("cols") && cfg["cols"].is_number_integer()) cols = cfg["cols"].get<int>();
    if (cols < 1) cols = 1;

    if (!cfg.contains("keys") || !cfg["keys"].is_array() || cfg["keys"].empty()) {
        throw std::runtime_error("keyboard_screen requires a non-empty \"keys\" array");
    }
    c->key_storage.reserve(cfg["keys"].size());
    for (const auto &k : cfg["keys"]) {
        if (!k.is_string()) throw std::runtime_error("keyboard_screen \"keys\" entries must be strings");
        c->key_storage.push_back(k.get<std::string>());
    }

    if (cfg.contains("keys_to_values") && cfg["keys_to_values"].is_object()) {
        for (auto it = cfg["keys_to_values"].begin(); it != cfg["keys_to_values"].end(); ++it) {
            if (it.value().is_string()) c->values[it.key()] = it.value().get<std::string>();
        }
    }

    if (cfg.contains("return_after_n_chars") && cfg["return_after_n_chars"].is_number_integer()) {
        c->return_after = cfg["return_after_n_chars"].get<int>();
    }
    c->total = c->return_after;
    bool show_save = cfg.value("show_save_button", false);
    bool show_cursor_keys = cfg.value("show_cursor_keys", true);
    if (cfg.contains("title_keystroke_template") && cfg["title_keystroke_template"].is_string()) {
        c->title_template = cfg["title_keystroke_template"].get<std::string>();
    }
    int max_length = 0;
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        max_length = cfg["max_length"].get<int>();
    }
    std::string initial_value = cfg.value("initial_value", std::string());

    const lv_font_t *key_font = &KEYBOARD_FONT;
    if (cfg.contains("keyboard_font") && cfg["keyboard_font"].is_string()) {
        std::string kf = cfg["keyboard_font"].get<std::string>();
        if (kf == "icon" || kf == "fontawesome") key_font = &ICON_FONT__SEEDSIGNER;
    }

    // Assemble the buttonmatrix map. Value keys wrap by `cols`; the control keys
    // (cursor left/right, backspace, optional save check) go on their OWN row
    // beneath — so a sparse value grid (e.g. coin flip) doesn't stretch the
    // controls across the width. The map char*s reference c->key_storage (fully
    // populated before any .c_str() is taken, so no realloc invalidates them) and
    // static icon/literal strings; c->ctrl has one entry per button (no "\n").
    struct cell_t { const char *txt; lv_buttonmatrix_ctrl_t ctrl; };
    std::vector<cell_t> controls;
    if (show_cursor_keys) {
        controls.push_back({SeedSignerIconConstants::CHEVRON_LEFT,  KSC(1)});
        controls.push_back({SeedSignerIconConstants::CHEVRON_RIGHT, KSC(1)});
    }
    controls.push_back({SeedSignerIconConstants::DELETE, KSC(1)});       // always a backspace
    if (show_save) controls.push_back({SeedSignerIconConstants::CHECK, KSC(1)});

    c->map.clear();
    c->ctrl.clear();
    const int value_count = (int)c->key_storage.size();
    int col = 0;
    for (int i = 0; i < value_count; ++i) {
        c->map.push_back(c->key_storage[i].c_str());
        c->ctrl.push_back(KSW(1));
        if (++col == cols && i + 1 < value_count) { c->map.push_back("\n"); col = 0; }
    }
    c->map.push_back("\n");
    for (const cell_t &cc : controls) { c->map.push_back(cc.txt); c->ctrl.push_back(cc.ctrl); }
    c->map.push_back("");

    const int value_rows = (value_count + cols - 1) / cols;
    const int total_rows = value_rows + 1;  // + the control row

    // Text-entry strip: the shared cursor-styled box (the entry's source of truth).
    lv_obj_t *ta = kb_make_text_entry(body, content_w, seedsigner_lvgl_is_static_render());
    if (!initial_value.empty()) lv_textarea_set_text(ta, initial_value.c_str());
    if (max_length > 0) lv_textarea_set_max_length(ta, (uint32_t)max_length);
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    c->ta = ta;

    // Keyboard: a plain buttonmatrix styled like the passphrase keyboard.
    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, c->map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, c->ctrl.data());
    kb_style_matrix(matrix, key_font);
    c->matrix = matrix;

    // Optional guidance text (e.g. the coin-flip "Heads = 1 / Tails = 0" legend),
    // centered below the keyboard. The caller passes it already translated; embedded
    // newlines split it into lines (Python renders these as stacked TextAreas). Its
    // height is reserved from the keyboard's vertical budget below so it never gets
    // pushed off-screen by the (capped, but still tall) keys.
    std::string guidance_text = cfg.value("guidance_text", std::string());
    int32_t guidance_h = 0;
    if (!guidance_text.empty()) {
        int lines = 1 + (int)std::count(guidance_text.begin(), guidance_text.end(), '\n');
        guidance_h = lines * (int32_t)lv_font_get_line_height(&BODY_FONT)
                     + (lines - 1) * BODY_LINE_SPACING + 2 * COMPONENT_PADDING;
    }

    // Cap the per-key size so a sparse grid stays a tidy block of comfortably large
    // targets instead of stretching to fill the screen (e.g. the lone backspace on a
    // coin-flip keyboard). Center the grid horizontally, just below the text entry.
    const int32_t kb_top  = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t avail_w = content_w;
    const int32_t avail_h = content_h - kb_top - guidance_h;
    const int32_t max_key = BUTTON_HEIGHT * 2;
    int32_t key_w = std::min(avail_w / cols, max_key);
    int32_t key_h = std::min(avail_h / total_rows, max_key);
    lv_obj_set_size(matrix, key_w * cols, key_h * total_rows);
    lv_obj_align(matrix, LV_ALIGN_TOP_MID, 0, kb_top);

    lv_obj_add_event_cb(matrix, keyboard_value_changed_cb, LV_EVENT_VALUE_CHANGED, c);

    if (!guidance_text.empty()) {
        lv_obj_t *guide = lv_label_create(body);
        lv_label_set_text(guide, guidance_text.c_str());
        lv_obj_set_width(guide, content_w);
        lv_obj_set_style_text_font(guide, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(guide, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_align(guide, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        // Match regular body text's line-to-line spacing for the multi-line legend.
        lv_obj_set_style_text_line_space(guide, BODY_LINE_SPACING, LV_PART_MAIN);
        lv_obj_align(guide, LV_ALIGN_TOP_MID, 0, kb_top + key_h * total_rows + COMPONENT_PADDING);
    }

    // keyboard_update_title re-stages the top-nav title for the current counter value
    // (the initial call lays it out; subsequent keystrokes keep it correct as the
    // counter widens). Static titles keep top_nav's own layout.
    keyboard_update_title(c);

    if (hardware) {
        // Joystick: the matrix is the group-focused object so its own directional
        // navigation drives key selection; the back button is the other member for
        // the top-nav handoff (mirrors the passphrase screen, sans side panel).
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, keyboard_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(screen.top_back_btn, keyboard_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(c->group);

        // Pre-select the first key so the joystick selection is visible immediately;
        // static-render adds PRESSED so the highlight shows in screenshots.
        lv_buttonmatrix_set_selected_button(matrix, 0);
        if (seedsigner_lvgl_is_static_render()) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, keyboard_cleanup_cb, LV_EVENT_DELETE, c);
    load_screen_and_cleanup_previous(screen.screen);
}
