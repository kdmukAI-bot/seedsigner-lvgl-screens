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
// settings_locale_picker_screen
// ---------------------------------------------------------------------------
//
// The language-selection screen (SETTING__LOCALE). Unlike a generic
// button_list_screen it must show every onboard language's name in its OWN native
// script on one screen — which the native path cannot do as live text (one active
// locale, one set of role fonts, and keeping N script fonts resident would blow
// the ESP32 glyph-cache pool). So each row is EITHER:
//   - live text (the always-resident baseline font) — for Latin-script endonyms
//     covered by the baked floor (English, Español, …); OR
//   - a pre-rendered A8 endonym image (native script, zero runtime font) — for
//     every language that ships a font pack (CJK, Arabic/Persian, Devanagari, …).
// A row is an image row iff its cfg carries an "image" filename; the host fetches
// that blob through the endonym image provider (locale_picker_set_image_provider,
// the same seam as ss_load_locale). Otherwise it is a live text row.
//
// Selection uses the standard body-button result path: clicking / entering a row
// fires seedsigner_lvgl_on_button_selected(row_index, ...); the host maps the
// index back to the locale it placed at that position (and persists / re-renders).
//
// Each row is a SINGLE line: the English name, then the native name — live text
// for a Latin-script native (e.g. "Spanish  Español"), or the pre-rendered image
// drawn right after the English text for a non-Latin script (e.g. "Hindi  हिन्दी").
// A row that overflows scrolls like any button-list row. The host orders the rows
// (English pinned first, the rest alphabetical by English name).
//
// cfg:
//   { "top_nav": {"title": "...", "show_back_button": true},
//     "active_locale": "<code>",              // radio-checked + initially focused
//     "rows": [ {"locale":"en","english":"English","native":"English"},            // live
//               {"locale":"es","english":"Spanish","native":"Español"},            // live
//               {"locale":"hi","english":"Hindi","native":"हिन्दी","image":true}, // image
//               ... ] }
void settings_locale_picker_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("rows") || !cfg["rows"].is_array()) {
        throw std::runtime_error("settings_locale_picker_screen: \"rows\" is required and must be an array");
    }
    const json &rows = cfg["rows"];
    if (rows.size() > SEEDSIGNER_SCAFFOLD_MAX_BUTTONS) {
        throw std::runtime_error("settings_locale_picker_screen: rows exceed SEEDSIGNER_SCAFFOLD_MAX_BUTTONS");
    }

    const std::string active_locale = cfg.value("active_locale", std::string());

    // No "button_list" key ⇒ scaffold Mode 1: top_nav + a plain scrollable body
    // (upper_body == body). We build the row buttons into that body ourselves so
    // each can host live text OR an endonym image. A pure list, so navigation uses
    // item-focus scrolling (no scroll-then-buttons hand-off).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    std::vector<lv_obj_t *> items;
    items.reserve(rows.size());
    lv_obj_t *prev = nullptr;
    size_t active_index = 0;

    for (size_t i = 0; i < rows.size(); ++i) {
        const json &row = rows[i];
        if (!row.is_object() || !row.contains("locale") || !row["locale"].is_string()) {
            throw std::runtime_error("settings_locale_picker_screen: each row needs a string \"locale\"");
        }
        const std::string locale  = row["locale"].get<std::string>();
        const std::string english = row.value("english", std::string());
        const std::string native  = row.value("native",  std::string());
        const bool is_active = (!active_locale.empty() && locale == active_locale);

        // Non-Latin natives ship a pre-rendered image; name it explicitly
        // ("image":"endonym_480.bin") or let the screen derive it for the active
        // profile height ("image":true → endonym_<height>.bin, so one scenario
        // renders at every resolution).
        std::string image_file;
        if (row.contains("image")) {
            if (row["image"].is_string()) {
                image_file = row["image"].get<std::string>();
            } else if (row["image"].is_boolean() && row["image"].get<bool>()) {
                image_file = "endonym_" + std::to_string(active_profile().height) + ".bin";
            }
        }

        // Row text = English name, a "|" separator, then the native name on the same
        // line. A non-Latin native is drawn as its image just after the separator, so
        // its label ends at "English |". A Latin native is appended as live text.
        // English itself (native == English) shows once, with no separator.
        const std::string primary = !english.empty() ? english : native;
        std::string label_text = primary;
        if (!image_file.empty()) {
            label_text = primary + " |";                 // native image drawn after the pipe
        } else if (!native.empty() && native != primary) {
            label_text = primary + " | " + native;       // Latin native as live text
        }

        // Radio row: the current locale is CHECK-marked; every row left-aligned and
        // chained below the previous (the same geometry button_list() produces).
        button_opts_t opts = {};
        opts.text             = label_text.c_str();
        opts.align_to         = prev;
        opts.is_text_centered = false;
        opts.icon_color       = SEEDSIGNER_ICON_COLOR_DEFAULT;
        opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;
        opts.style            = BUTTON_STYLE_CHECKED_SELECTION;
        opts.is_checked       = is_active;
        lv_obj_t *btn = button_ex(screen.body, &opts);

        // Image row (native-script name, no runtime font). On any failure the row
        // still shows its English name: fail soft, never crash on a bad pack image.
        if (!image_file.empty()) {
            locale_picker_attach_endonym(btn, locale.c_str(), image_file.c_str());
        }

        items.push_back(btn);
        prev = btn;
        if (is_active) active_index = i;
    }

    // Focus (and scroll to) the current locale's row by default; an explicit
    // cfg.initial_selected_index still overrides via nav_initial_index_from_cfg.
    bind_screen_navigation(
        cfg, screen,
        items.empty() ? nullptr : items.data(), items.size(),
        NAV_BODY_VERTICAL, active_index);

    load_screen_and_cleanup_previous(screen.screen);
}
