// version_screen
//
// Python provenance: VersionScreen (settings_screens.py), a BaseTopNavScreen.
//
// Settings > Version: an informational top-nav screen showing the running build.
// Distinct from the opening splash (which shows only the version name): this screen
// stacks the version name, an optional "fork:" row and "commit:" row, and pins the
// build timestamp to the bottom-center. The app nulls the fork + commit for release
// images, so this screen hides those rows when they are absent.
//
// Layout (mirrors Python VersionScreen.__post_init__):
//   - version_name: fixed-width, ACCENT_COLOR, hard-wrapped to the panel width
//     (version strings have no spaces to break on). Top-anchored 2*COMPONENT_PADDING
//     below the top nav when a fork/commit row follows; vertically CENTERED on the
//     screen when neither is present (the release-image case).
//   - fork/commit rows: a gray "fork:" / "commit:" label (BODY_FONT, LABEL_FONT_COLOR)
//     and its fixed-width value (BODY_FONT_COLOR). The labels are right-aligned to a
//     shared column so their colons line up and the values start at the same x, and
//     the whole label+value block is CENTERED on the screen (Python left-justifies at
//     EDGE_PADDING, which skews too far left on the wide landscape panels). Label and
//     value are baseline-aligned across their differing fonts.
//   - timestamp: BODY_FONT, centered, pinned to the bottom of the screen.
//
// The screen has no body focusables — only the top-nav back button — so it returns
// only through the top-nav navigation callbacks (back -> SEEDSIGNER_RET_BACK_BUTTON).
// Lifecycle Tier 1 (stateless): no statics, timers, or heap ctx.
//
// FONT NOTE (parity caveat, same situation as tools_calc_final_word_screen): Python
// renders version_name in Inconsolata-Regular at top_nav_title_font_size + 6 (26 px)
// and the fork/commit values in Inconsolata-SemiBold at top_nav_title_font_size
// (20 px). This repo bakes Inconsolata-SemiBold ONLY, at 22 px (CANDIDATE_FONT) and
// 24 px (KEYBOARD_FONT) — no Regular weight and no 20/26 px variants. So version_name
// uses KEYBOARD_FONT (24 px, closest to 26) and the values use CANDIDATE_FONT (22 px,
// closest to 20). Both are the nearest available fixed-width faces; exact PIL pixel
// parity is not the bar for these migrated screens (identical output across the two
// platforms for a given cfg is).
//
// cfg:
//   top_nav.title             (string, required)     localized screen title (read by
//            the scaffold; enforced via require_top_nav_title; Python: _("Version")).
//   top_nav.show_back_button  (bool, default true)   Python BaseTopNavScreen default.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   version_name              (string, required)     Version.get_version_name(); the
//            screen wraps it to the panel width (fixed-width font, ACCENT_COLOR).
//   version_fork              (string, optional)     fork owner; omit or "" -> no
//            "fork:" row (the app nulls it for release images).
//   short_commit_hash         (string, optional)     short commit hash; omit or "" ->
//            no "commit:" row (the app nulls it for release images).
//   version_timestamp         (string, required)     host pre-formatted display string
//            ("%Y-%m-%d %H:%M:%S UTC"); a datetime is not JSON-native and the format
//            is fixed, so no timestamp formatting happens on-device.
//   input.mode / input.keys.* (optional)             navigation-layer overrides.
//   allow_screensaver         (bool, default true)   per-screen screensaver policy.

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // version_screen decl, screen_scaffold_t
#include "gui_constants.h"    // COMPONENT_PADDING, TOP_NAV_HEIGHT, ACCENT_COLOR, LABEL_FONT_COLOR, BODY_FONT_COLOR, BODY_FONT, KEYBOARD_FONT, CANDIDATE_FONT
#include "navigation.h"       // NAV_BODY_VERTICAL, NAV_INDEX_NONE (no-buttons bind form)
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_label + per-object style setters, lv_text_get_size, layout placement

#include <nlohmann/json.hpp>  // json (cfg reads)

#include <cstdint>            // uint32_t (byte offsets)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


namespace {

// Step past one UTF-8 codepoint at byte `i`, returning the next codepoint's byte
// offset (clamped to `total`). Version strings are ASCII in practice; this keeps the
// wrap codepoint- (not byte-) accurate to match Python `str` slicing.
uint32_t version_utf8_step(const char *s, uint32_t i, uint32_t total) {
    if (i >= total) return total;
    ++i;  // lead byte
    while (i < total && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;  // continuation bytes
    return i;
}

// Wrap `s` into lines of at most `max_chars` codepoints, joined by '\n'. A string
// that already fits stays a single line (Python only wraps when the name is wider
// than the usable width). Version strings have no spaces, so '-' is the only natural
// break: when a dash falls within the line window we break AFTER it, so a dashed name
// like "feature/version-display" breaks as "feature/version-" / "display" rather than
// mid-word ("feature/version-di" / "splay"). A window with no dash (or a single
// over-long segment) hard-splits at max_chars so it can never overflow.
std::string wrap_version_name(const std::string &s, int max_chars) {
    if (max_chars <= 0) return s;  // defensive: unmeasurable width -> no wrap

    const char *p = s.c_str();
    const uint32_t total = (uint32_t)s.size();
    std::string out;
    uint32_t start = 0;
    bool first = true;

    while (start < total) {
        // Walk up to max_chars codepoints from `start`, remembering the byte offset
        // just past the last '-' seen (the last in-window break opportunity) and where
        // an exact max_chars cut would land (the hard-split fallback).
        uint32_t cursor = start;
        uint32_t last_break = start;   // == start means "no dash seen yet"
        for (int cp = 0; cp < max_chars && cursor < total; ++cp) {
            const bool is_dash = (p[cursor] == '-');  // '-' is ASCII, one byte
            cursor = version_utf8_step(p, cursor, total);
            if (is_dash) last_break = cursor;         // break AFTER the dash
        }

        uint32_t line_end;
        if (cursor >= total)          line_end = total;       // remainder fits
        else if (last_break > start)  line_end = last_break;  // break after last dash
        else                          line_end = cursor;      // no dash -> hard cut

        if (!first) out += '\n';
        out.append(s, start, line_end - start);
        first = false;
        start = line_end;
    }
    return out;
}

// Pixel width of `text` in `font` (no wrapping), via the shared metric path.
int32_t measure_text_width(const char *text, const lv_font_t *font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// Read an optional string cfg field; returns "" when absent, non-string, or empty.
// Empty and absent are equivalent here (the app nulls fork/hash for release images).
std::string optional_string(const json &cfg, const char *key) {
    if (cfg.contains(key) && cfg[key].is_string()) {
        return cfg[key].get<std::string>();
    }
    return "";
}

}  // namespace


void version_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields. version_name and version_timestamp are user-visible content the
    // host always supplies (the name from Version.get_version_name(); the timestamp
    // pre-formatted host-side, since a datetime is not JSON-native). Throw before the
    // scaffold exists so no throw path can leak LVGL objects.
    if (!cfg.contains("version_name") || !cfg["version_name"].is_string()) {
        throw std::runtime_error("version_screen: version_name is required and must be a string");
    }
    if (!cfg.contains("version_timestamp") || !cfg["version_timestamp"].is_string()) {
        throw std::runtime_error("version_screen: version_timestamp is required and must be a string");
    }
    const std::string version_name = cfg["version_name"].get<std::string>();
    const std::string timestamp    = cfg["version_timestamp"].get<std::string>();

    // Optional rows: the app passes these in local dev and nulls them for release
    // images. Empty or absent => the row is hidden.
    const std::string version_fork = optional_string(cfg, "version_fork");
    const std::string commit_hash  = optional_string(cfg, "short_commit_hash");
    const bool has_fork   = !version_fork.empty();
    const bool has_commit = !commit_hash.empty();
    const bool has_rows   = has_fork || has_commit;

    // Structural defaults (write-if-absent). Python BaseTopNavScreen:
    // show_back_button=True, show_power_button=False. The localized title is content
    // and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "version_screen");

    // --- Scaffold ---

    // Fixed layout (not scrollable): the timestamp is pinned to the screen bottom and
    // the stacked rows always fit, so nothing scrolls (Python's BaseTopNavScreen is a
    // fixed layout too).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // The body spans from just below the top nav to the screen bottom, with
    // EDGE_PADDING left/right; content coords are relative to that padded box.
    const int32_t content_width = lv_obj_get_content_width(screen.body);

    // --- Version name (fixed-width, accent, wrapped to width) ---

    const lv_font_t *name_font = &KEYBOARD_FONT;  // 24 px Inconsolata-SemiBold (see FONT NOTE)
    const int32_t name_char_width = measure_text_width("X", name_font);  // fixed-width -> any glyph
    const int max_name_chars = (name_char_width > 0) ? (int)(content_width / name_char_width) : 0;
    const std::string wrapped_name = wrap_version_name(version_name, max_name_chars);

    lv_obj_t *name_label = lv_label_create(screen.body);
    lv_label_set_text(name_label, wrapped_name.c_str());
    lv_obj_set_style_text_font(name_label, name_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (has_rows) {
        // Top-anchored 2*COMPONENT_PADDING below the top nav (Python:
        // top_nav.height + 2*COMPONENT_PADDING; the body already starts at the nav
        // bottom, so the offset is body-relative).
        lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 2 * COMPONENT_PADDING);
    } else {
        // No fork/commit (release image): vertically center on the SCREEN, like
        // Python (canvas_height/2). The body's center sits TOP_NAV_HEIGHT/2 below the
        // screen center, so shift up by that to land on the true screen center.
        lv_obj_align(name_label, LV_ALIGN_CENTER, 0, -TOP_NAV_HEIGHT / 2);
    }

    // --- Optional fork / commit rows ---

    // A row is a gray "<label>:" (BODY_FONT) plus its fixed-width value. The labels
    // are right-aligned to a shared column so their COLONS line up and the two values
    // start at the same x, and the whole label+value block is CENTERED on the screen.
    // (Python left-justifies at EDGE_PADDING, which skews hard to the left on the wide
    // landscape panels; centering the colon-aligned group reads better across aspect
    // ratios while keeping Python's colon alignment.)
    if (has_rows) {
        const lv_font_t *label_font = &BODY_FONT;
        const lv_font_t *value_font = &CANDIDATE_FONT;  // 22 px Inconsolata-SemiBold (see FONT NOTE)

        // Label column = the widest PRESENT label (no trailing space; a single
        // COMPONENT_PADDING separates the colon from the value — Python's "fork: "
        // trailing space + padding read too wide).
        const int32_t fork_label_w   = measure_text_width("fork:", label_font);
        const int32_t commit_label_w = measure_text_width("commit:", label_font);
        int32_t label_col_w = 0;
        if (has_fork)   label_col_w = fork_label_w;
        if (has_commit) label_col_w = (commit_label_w > label_col_w) ? commit_label_w : label_col_w;

        // Center the block: [label column][COMPONENT_PADDING][widest value].
        const int32_t fork_value_w   = has_fork   ? measure_text_width(version_fork.c_str(), value_font) : 0;
        const int32_t commit_value_w = has_commit ? measure_text_width(commit_hash.c_str(),  value_font) : 0;
        const int32_t max_value_w    = (fork_value_w > commit_value_w) ? fork_value_w : commit_value_w;
        const int32_t block_width    = label_col_w + COMPONENT_PADDING + max_value_w;
        int32_t block_left = (content_width - block_width) / 2;
        if (block_left < 0) block_left = 0;             // clamp if the block is wider than the content
        const int32_t value_x = block_left + label_col_w + COMPONENT_PADDING;

        // The label and value use different fonts/sizes, so top-anchoring them would
        // misalign their baselines (visible at the larger profiles). Shift each down so
        // both sit on a shared baseline = the taller font's ascent below the row top.
        const int32_t label_ascent = (int32_t)lv_font_get_line_height(label_font) - label_font->base_line;
        const int32_t value_ascent = (int32_t)lv_font_get_line_height(value_font) - value_font->base_line;
        const int32_t row_ascent   = (label_ascent > value_ascent) ? label_ascent : value_ascent;
        const int32_t label_dy     = row_ascent - label_ascent;
        const int32_t value_dy     = row_ascent - value_ascent;
        const int32_t row_height   = row_ascent +
            ((label_font->base_line > value_font->base_line) ? label_font->base_line : value_font->base_line);

        // Place one right-aligned label + left-aligned value pair, baseline-aligned.
        auto place_row = [&](const char *label_text, int32_t label_w, const std::string &value, int32_t y) {
            lv_obj_t *label = lv_label_create(screen.body);
            lv_label_set_text(label, label_text);
            lv_obj_set_style_text_font(label, label_font, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_pos(label, block_left + label_col_w - label_w, y + label_dy);

            lv_obj_t *val = lv_label_create(screen.body);
            lv_label_set_text(val, value.c_str());
            lv_obj_set_style_text_font(val, value_font, LV_PART_MAIN);
            lv_obj_set_style_text_color(val, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_pos(val, value_x, y + value_dy);
        };

        // Rows begin 3*COMPONENT_PADDING below the version name (Python spacing). The
        // name is TOP_MID at 2*CP, so its laid-out bottom is available after layout.
        lv_obj_update_layout(screen.body);
        int32_t row_y = lv_obj_get_y(name_label) + lv_obj_get_height(name_label) + 3 * COMPONENT_PADDING;

        if (has_fork) {
            place_row("fork:", fork_label_w, version_fork, row_y);
            row_y += row_height + COMPONENT_PADDING;
        }
        if (has_commit) {
            place_row("commit:", commit_label_w, commit_hash, row_y);
        }
    }

    // --- Timestamp (pinned bottom-center) ---

    lv_obj_t *timestamp_label = lv_label_create(screen.body);
    lv_label_set_text(timestamp_label, timestamp.c_str());
    lv_obj_set_style_text_font(timestamp_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(timestamp_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    // The body's bottom pad is COMPONENT_PADDING (== EDGE_PADDING at every profile),
    // so aligning to the content-box bottom reproduces Python's EDGE_PADDING gap.
    lv_obj_align(timestamp_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    // --- Navigation + load ---

    // No body focusables: only the top-nav back button is reachable, so keep the
    // literal (NULL, 0, NAV_INDEX_NONE) form (same contract as donate_screen).
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
