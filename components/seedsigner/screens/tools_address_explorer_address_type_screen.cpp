// tools_address_explorer_address_type_screen
//
// Python provenance: ToolsAddressExplorerAddressTypeScreen (tools_screens.py, a
// ButtonListScreen subclass).
//
// The Address Explorer's "which addresses?" chooser: a bottom-pinned button list
// [Receive, Change] under a context header that identifies WHERE the addresses come
// from. The header takes one of two mutually-exclusive shapes (Python: `if
// self.fingerprint: ... else: ...`), and it is the header — a labeled-value stack the
// generic button_list_screen's single plain intro-text block can't express — that makes
// this need its own entry point:
//
//   - Single-sig seed (fingerprint present): two LEFT-aligned IconTextLine rows,
//     top-anchored under the nav —
//       Fingerprint : <fingerprint>   (FINGERPRINT glyph, INFO_COLOR blue)
//       Derivation  : <derivation>    (DERIVATION glyph, DEFAULT color — see below)
//     where <derivation> is the script-type display name, or the custom derivation
//     path when a custom derivation is in use (the host composes + localizes either).
//
//   - Loaded wallet descriptor (no fingerprint): a single CENTERED, icon-less
//     IconTextLine — "Wallet descriptor" over its display name (e.g. "2 / 3 multisig").
//
//   - Neither: no header (the buttons alone). Python always renders one of the two, but
//     the header is contextual, not this screen's reason to exist (the choice is), so a
//     header-less call is tolerated rather than a hard error.
//
// Faithful Python asymmetry: the Fingerprint row sets icon_color = INFO_COLOR, but the
// Derivation row sets NO icon_color, so its glyph falls through to the default
// BODY_FONT_COLOR (white). This differs from seed_export_xpub_details_screen, where BOTH
// rows are INFO_COLOR — reproduced here exactly as tools_screens.py writes it.
//
// Layout notes: the fingerprint header is the seed_export_xpub_details_screen idiom (a
// shared icon column so the two text columns align, top-anchored + left-aligned under the
// nav, buttons bottom-pinned) — minus the xpub line and the warning edges. Python spaces
// the two rows 2*COMPONENT_PADDING apart (wider than xpub-details' 1.5x), reproduced here
// via the flex row-gap since there are only two rows and the page never overflows.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python __post_init__: _("Address Explorer")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   button_list               (array, required, non-empty)  the localized choices
//            (Python: "Receive addresses", "Change addresses").
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   -- header, one of the two shapes (all header strings are localized by the host) --
//   fingerprint               (string, seed source)  BIP-32 master fingerprint (hex).
//            When present, ALL of the fingerprint-header fields below are required.
//   fingerprint_label         (string, req. w/ fingerprint)  field caption
//            (Python: _("Fingerprint")).
//   derivation_text           (string, req. w/ fingerprint)  the derivation value — the
//            localized script-type display name or the custom derivation path.
//   derivation_label          (string, req. w/ fingerprint)  field caption
//            (Python: _("Derivation")).
//   wallet_descriptor_text    (string, descriptor source)  the descriptor display name
//            (e.g. "2 / 3 multisig"); used only when `fingerprint` is absent.
//   wallet_descriptor_label   (string, req. w/ wallet_descriptor_text)  field caption
//            (Python: _("Wallet descriptor")).
//   initial_selected_index    (int, optional)        overrides the default focus of 0
//            (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // tools_address_explorer_address_type_screen decl, screen_scaffold_t
#include "components.h"       // icon_text_line + icon_text_line_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // EDGE_PADDING, COMPONENT_PADDING, INFO_COLOR, ICON_FONT_SIZE, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // upper_body flex grow/align/pad setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


namespace {

// Small helper: read a required localized string field of the active header shape,
// throwing the standard screen-prefixed error when the host omitted it. Kept local —
// the header's "present X implies required Y/Z" contract is specific to this screen.
std::string require_header_string(const json &cfg, const char *key) {
    if (!cfg.contains(key) || !cfg[key].is_string()) {
        throw std::runtime_error(std::string("tools_address_explorer_address_type_screen: ")
                                 + key + " is required (and must be a string) for this header");
    }
    return cfg[key].get<std::string>();
}

}  // namespace


void tools_address_explorer_address_type_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: button_list is the choice this screen exists to offer, and is
    // user-visible CONTENT that always arrives localized from the host view layer.
    // Validated before the scaffold exists so no throw path can leak LVGL objects.
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("tools_address_explorer_address_type_screen: button_list is required and must be a non-empty array");
    }

    // Header shape selection (mirrors Python's `if self.fingerprint: ... else: ...`).
    // The presence of `fingerprint` picks the two-row seed header; otherwise
    // `wallet_descriptor_text` picks the single centered descriptor row; otherwise no
    // header. When a shape is chosen its localized captions/values are REQUIRED — a
    // partial header is a host bug, so it throws (content policy) rather than rendering
    // a half-built row. Read the strings now, before the scaffold exists.
    bool has_fingerprint_header = cfg.contains("fingerprint") && cfg["fingerprint"].is_string();
    bool has_descriptor_header  = !has_fingerprint_header &&
                                  cfg.contains("wallet_descriptor_text") &&
                                  cfg["wallet_descriptor_text"].is_string();

    std::string fingerprint, fingerprint_label, derivation_text, derivation_label;
    std::string wallet_descriptor_text, wallet_descriptor_label;
    if (has_fingerprint_header) {
        fingerprint       = cfg["fingerprint"].get<std::string>();
        fingerprint_label = require_header_string(cfg, "fingerprint_label");
        derivation_text   = require_header_string(cfg, "derivation_text");
        derivation_label  = require_header_string(cfg, "derivation_label");
    } else if (has_descriptor_header) {
        wallet_descriptor_text  = cfg["wallet_descriptor_text"].get<std::string>();
        wallet_descriptor_label = require_header_string(cfg, "wallet_descriptor_label");
    }

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False. The
    // localized title is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "tools_address_explorer_address_type_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Grow upper_body to claim the whole band above the bottom-pinned buttons and
    // collapse the scaffold spacer, so the header sits in a container sized to fit (the
    // seed_finalize / xpub-details idiom). The header's own flex alignment differs by
    // shape (set below): the seed header is top-anchored + LEFT-aligned; the descriptor
    // header is top-anchored + horizontally CENTERED.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    if (has_fingerprint_header) {
        // Two left-aligned IconTextLine rows, top-anchored under the nav (Python
        // screen_x = EDGE_PADDING, first row at top_nav.height + COMPONENT_PADDING).
        // A shared icon column (icon_width = ICON_FONT_SIZE) aligns the two text
        // columns; the 2*COMPONENT_PADDING flex row-gap matches Python's inter-row gap.
        lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_set_style_pad_left(screen.upper_body, EDGE_PADDING, LV_PART_MAIN);
        lv_obj_set_style_pad_row(screen.upper_body, 2 * COMPONENT_PADDING, LV_PART_MAIN);

        // 1. Fingerprint row — FINGERPRINT glyph in INFO_COLOR blue.
        icon_text_line_opts_t fingerprint_opts = {};
        fingerprint_opts.icon_glyph  = SeedSignerIconConstants::FINGERPRINT;
        fingerprint_opts.icon_color  = INFO_COLOR;
        fingerprint_opts.label_text  = fingerprint_label.c_str();
        fingerprint_opts.value_text  = fingerprint.c_str();
        fingerprint_opts.label_color = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
        fingerprint_opts.value_color = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
        fingerprint_opts.icon_width  = ICON_FONT_SIZE;                  // shared icon column -> aligned text
        icon_text_line(screen.upper_body, &fingerprint_opts);

        // 2. Derivation row — DERIVATION glyph in the DEFAULT color (white). Python sets
        //    no icon_color on this row (unlike the fingerprint row above) — faithful.
        icon_text_line_opts_t derivation_opts = {};
        derivation_opts.icon_glyph  = SeedSignerIconConstants::DERIVATION;
        derivation_opts.icon_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> BODY_FONT_COLOR (white), Python-faithful
        derivation_opts.label_text  = derivation_label.c_str();
        derivation_opts.value_text  = derivation_text.c_str();
        derivation_opts.label_color = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> LABEL_FONT_COLOR (gray)
        derivation_opts.value_color = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> BODY_FONT_COLOR (white)
        derivation_opts.icon_width  = ICON_FONT_SIZE;                   // same column as fingerprint
        icon_text_line(screen.upper_body, &derivation_opts);

    } else if (has_descriptor_header) {
        // Single centered, icon-less IconTextLine (Python is_text_centered=True): the
        // whole row is centered horizontally (cross-axis CENTER), with the label centered
        // over its value inside it. Top-anchored at top_nav.height + COMPONENT_PADDING.
        lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);

        icon_text_line_opts_t descriptor_opts = {};
        descriptor_opts.label_text       = wallet_descriptor_label.c_str();
        descriptor_opts.value_text       = wallet_descriptor_text.c_str();
        descriptor_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
        descriptor_opts.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
        descriptor_opts.is_text_centered = true;                           // no icon -> centers label over value
        icon_text_line(screen.upper_body, &descriptor_opts);
    }
    // else: no header — the buttons alone (upper_body stays empty).

    // --- Navigation + load ---

    // Menu-style default index: a choice list always has a selection, so the first
    // button (Receive) starts focused (the host may override via initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
