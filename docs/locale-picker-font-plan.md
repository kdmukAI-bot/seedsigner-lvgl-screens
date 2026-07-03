# Locale picker — font rendering plan (rough outline)

_Status: **outline only, not yet designed.** Captured from a cross-repo discussion during the
`seedsigner`-side button-list migration session (2026-06-25). This is a starting sketch for the
screens-repo owner to build into a robust plan — the open questions below are real decisions, not
settled. Companion context: `docs/font-memory-plan.md`, `docs/font-tiering-plan.md`,
`components/seedsigner/locale_fonts.cpp`._

## The problem

The locale picker (`SETTING__LOCALE`) shows **every onboard language's name in its own native
script, all on one screen** (Japanese in JP, Korean in KR, Arabic in AR, Hindi in Devanagari, …,
plus the Latin-script languages). On CPython/PIL this works today because PIL loads a per-language
TTF at render time (`GUIConstants.get_button_font_name(locale)` in the `seedsigner` repo). The native
LVGL path **cannot reproduce this as a generic `button_list_screen`**, on two independent axes.

## Why the generic `button_list_screen` can't do it

1. **API — one active locale, screen-wide role fonts, no per-button font.**
   `font_registry.cpp` holds a single `g_current_locale`; you `seedsigner_set_locale(x)` → render →
   clear/retire/reap. Button labels draw with `&BUTTON_FONT` (`components.cpp`), a profile-level
   pointer; `button_item_cfg_t` has no `font` field. (A fallback chain could render mixed scripts
   without a per-button field, but see #2.)
2. **Memory — the real wall, ESP32 only.** Tiny TTF glyph-cache **index nodes** live in the small
   internal pool (P4 128 KB / S3 64 KB); the whole `feat/font-memory-dedup` effort exists just to fit
   **one** locale. ~7–11 primary/shaped scripts resident at once would blow the pool →
   `LV_ASSERT_HANDLER` `while(1)` → watchdog reboot, **no graceful degradation**. A fallback chain
   doesn't escape this — every fallback target must be resident, so the cost is identical.
   (Pi Zero's 512 MB wouldn't OOM, but the API wall still applies and we want one set of screens.)

See `docs/font-memory-plan.md` and `docs/knowledge/tiny-ttf-cache-spin-root-cause.md` for the pool
mechanics.

## Proposed approach — prerendered label images + live Latin

Remove runtime fonts from the picker entirely for the expensive scripts:

- **Offline (lang-pack build step):** rasterize each non-baseline endonym to a small **A8 alpha**
  bitmap and emit it as an `lv_image_dsc_t` (C array or `.bin`). Reuse the existing offline HarfBuzz
  shaping pipeline (the `runs.bin` machinery), so RTL/bidi and complex-script shaping are resolved at
  build time — the device does **zero** shaping for the picker.
- **Runtime — a bespoke `locale_picker_screen` with mixed rows:**
  - `chain: primary` scripts (CJK zh_Hans/zh_Hant/ja/ko, Arabic/Persian, shaped Hindi/Thai/Urdu, plus
    Bengali/Gujarati/Gurmukhi/Lao/Hebrew) → **prerendered A8 image** row.
  - default + **pure-Latin** endonyms → **live text** in the always-resident baseline font. All Latin
    rows share the one already-loaded font instance → **zero new internal-pool pressure**.

This dodges both walls: image rows need no runtime font (no per-button-font API, no glyph-cache index
nodes); live Latin rows reuse the baked baseline. The split maps directly onto the **`chain` role
already in `locale_fonts.cpp`** (`primary` → image, `fallback`/default → live — modulo the open
question below).

## Format / sizing — proposed defaults (confirm)

- **A8 alpha**, not PNG, not full-color: tintable at runtime for the normal label color **and** the
  selected-row invert; anti-aliasing preserved (matters for CJK legibility at 18–20 px, which is why
  those role sizes are already bumped); blits straight from flash with no decode heap.
- **Per display-profile asset sets** (240/320/480 → their button-role sizes) so labels stay crisp;
  the firmware picks the set for its active profile. Storage is trivial (sub-MB even at A8 across
  profiles; far less if Latin rows stay live).

## Open questions to resolve into a robust plan

- **Baked baseline glyph coverage (verify first).** Does the firmware's baked default font (OpenSans)
  include **Cyrillic + Greek + Latin-Extended/Vietnamese**? The existence of separate `el` / `ru` /
  `vi` *fallback block-subset* packs suggests the baked baseline may be **Latin-only**. If so, those
  endonyms also need images (or widen the baked baseline — small, same-size subsets). Don't assume.
- **Screen home:** dedicated `locale_picker_screen` vs. extending the button-row label to accept an
  image source. Leaning dedicated — it's a fixed, finite, known-offline asset set, consistent with
  the other bespoke screens.
- **Row composition:** image label + the current-selection check/radio icon coexistence; centering /
  alignment; selected-state tint.
- **Tooling:** where the offline rasterizer lives in the lang-pack build; how assets regenerate when a
  language is added; manifest entries (extend the per-locale `manifest.json`?).
- **Rasterizer consistency:** offline rasterizer AA vs. live Tiny-TTF AA on the Latin rows — accept
  the hairline difference, or prerender all rows for uniformity? (Leaning accept; prerendering Latin
  gives up the savings.)
- **Scope reminder:** this is the **picker only**. Once a language is selected, every other screen
  still renders live via `set_locale → render`. Unaffected.

## References

- `components/seedsigner/font_registry.cpp` — single active locale; Tiny TTF runtime rasterizer.
- `components/seedsigner/locale_fonts.cpp` — chain roles (`primary` vs `fallback`): the split this
  plan keys on.
- `components/seedsigner/components.cpp` — button label font = `&BUTTON_FONT` (no per-item font).
- `docs/font-memory-plan.md`, `docs/font-tiering-plan.md`,
  `docs/knowledge/tiny-ttf-cache-spin-root-cause.md` — the memory constraints.
- `seedsigner` repo: `LocaleSelectionView` (`views/settings_views.py`) — the CPython picker that is
  **deferred** on the native path. Its migration is blocked on this plan, not on the font branch
  forwarding `font_name`/`font_size`.
