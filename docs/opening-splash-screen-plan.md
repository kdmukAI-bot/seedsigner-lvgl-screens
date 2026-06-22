# Opening splash screen — plan & as-built notes

## Goal / why
Bring the MicroPython (ESP32) boot experience up to parity with the Pi/Python build, which
shows an **opening splash** at startup: the SeedSigner logo, a `v{VERSION}` string, a logo
animation, and an optional HRF partner-logo band ("With support from:"). The MicroPython side
currently boots to a black screen. Per the **screens-lead → platform → integrate** flow, the
shared LVGL `splash_screen()` lands in this repo first (Part 1), then the
`seedsigner-micropython-builder` wires the binding + a C-boot logo (Part 2), then `seedsigner`
restores the app call-site (Part 3).

## Status
- **Part 1 (this repo): IMPLEMENTED + verified** — `splash_screen()`, a new multi-resolution
  image-asset pattern, the shared logo/HRF assets, scenarios, and tool wiring all build and
  render across all four display profiles.
- **Part 2 (builder): NOT STARTED.**
- **Part 3 (seedsigner app): NOT STARTED.**

## Load-bearing technical fact (drives the whole design)
LVGL screen functions here **build the UI and return** — they must NOT block for the splash's
several seconds. So timing/animation is driven by **`lv_anim` (logo fade/slide) + `lv_timer`
(reveal/hold sequence)**, and completion is emitted through the result queue via the weak extern
`seedsigner_lvgl_on_button_selected(code, label)` — the same pattern `screensaver_screen()` uses.
The host (`run_lvgl_screen` → poll loop) consumes the completion and proceeds.

The **screenshot generator** renders a single static frame (`seedsigner_lvgl_set_static_render(true)`).
So `splash_screen()` composes the **final frame at full opacity** and `return`s early before the
animation/timer code, which is gated behind `!g_static_render`.

---

## Part 1 — Shared LVGL `splash_screen()` (this repo)  ✅

### Multi-resolution IMAGE asset pattern (new — mirrors the fonts)
The logo scales **relative to the display** via the existing `PX_MULTIPLIER` system (100/133/200
for display heights 240/320/480), rather than a single fixed size. This introduces the same
multi-variant pattern the fonts already use:
- **Declarations** gated by `#ifdef SUPPORT_DISPLAY_HEIGHT_{240,320,480}` in `gui_constants.h`.
- **Runtime selectors** `seedsigner_logo_for_active_profile()` / `hrf_logo_for_active_profile()`
  in `gui_constants.cpp` (structural copies of `fonts_for_multiplier()`), keyed on
  `active_profile().px_multiplier`, each branch `#ifdef`-gated.
- **CMake** lists the variant `.c` per height: gated `SEEDSIGNER_IMAGE_SOURCES` in the four tool
  CMakeLists (`screenshot_generator`, `screen_runner`, `web_runner`, `runner_core/test`); flat in
  the IDF `components/seedsigner/CMakeLists.txt` (matches the fonts there).
- All three layers must stay in **lockstep** on the same `SUPPORT_DISPLAY_HEIGHT_*` triplet — a
  single-resolution build then compiles/links **only its own variant** (verified).
- **Image symbols use the accurate `_133x` suffix** (not the fonts' legacy `_150x`). A TODO in
  `components/seedsigner/TODO.md` tracks renaming the legacy `_150x` *font* labels to `_133x`
  across the three repos.

The SeedSigner wordmark set is **shared by both** `splash_screen()` and `screensaver_screen()`
(the screensaver was migrated to the selector), so the screensaver now also scales per resolution.

### Assets — baked RGB565 `.c`, committed (`components/seedsigner/images/`)
Prepped offline by `scripts/prep_splash_sources.py` (committed) → baked by `scripts/png_to_lvgl.py`.
Pre-flattened/cropped masters live in `components/seedsigner/images/src/` (committed).

**SeedSigner wordmark** — source `seedsigner_logo_transparent.png` (4000×1481, transparent):
- Prep: **remove exterior background** (the source is white-bg with imperfect alpha extraction —
  flood-fill from the border through transparent-or-whitish pixels, stopping at the orange ring, so
  the leftover white edge becomes transparent while the interior white SEED box / SIGNER text is
  preserved by connectivity) → composite on black → crop to content. Order matters: flatten/clean
  before any resample to avoid alpha-bleed at edges.
- Bake heights **70 / 93 / 140** (px_mult 100/133/200) → 219×70 / 290×93 / 437×140. The 70px base =
  Python's *effective* logo height (the 218×70 pill inside its 240² black square; Python's
  `logo_height = 70`).
- Symbols/files: `seedsigner_logo_img{,_133x,_200x}.c`.

**HRF partner logo** — source `HRF-Logo.webp` (2832×779, HRF's on-white version: pink H + BLACK text):
- Prep: **recolor** dark ink → white (preserve the pink H, by RGB chroma) → composite on black →
  crop → **pad to Python's `hrf_logo.png` framing** (content = 95%×87% of a 200×61-proportioned
  canvas). The padding makes the baked asset a faithful high-res analog of Python's asset so the
  splash's Python-mirroring layout lands identically (without it, the tightly-cropped logo rendered
  ~18% wider and 4px lower than Python). Master is 2981×909 (aspect 3.279).
- Bake heights **61 / 81 / 122** → 200×61 / 266×81 / 400×122 (visible content matches Python's
  190×53; ~2px wider purely from the new artwork's aspect).
- Symbols/files: `hrf_logo_img{,_133x,_200x}.c`.

### `splash_screen(void *ctx_json)` (`components/seedsigner/seedsigner.cpp`)
Modeled on `screensaver_screen()` (timer/indev/cleanup). Reuses `parse_screen_json_ctx` /
`cfg.merge_patch` defaults, `load_screen_and_cleanup_previous`, `text_top_leading`, the
`active_profile()` macros, and the two image selectors.

1. **Build the FINAL frame** (black bg, non-scrollable): logo centered, raised by
   `logo_offset_y = show_partner ? -(56·px_mult/100) : 0` (Python's −56 offset, scaled); version
   label below (`TOP_NAV_TITLE_FONT`, `ACCENT_COLOR`, positioned via Python's formula minus
   `text_top_leading`); partner band bottom-pinned (sponsor label `BODY_FONT` #cccccc + HRF image,
   laid out bottom-up like Python). All at full opacity.
2. **`boot_logo_only`**: render only the centered logo (matches the firmware C-boot logo position);
   no version/band/animation/completion. Returns early.
3. **Static render** (`g_static_render`): return after the build — the single still is the final frame.
4. **Live** (the timed reveal) — **unified "center, then up"** entrance, with version + band hidden
   until the logo settles (Python reveals the version only after the logo finishes):
   - Logo **enters centered**. CPython (`logo_already_shown=false`) **fades** it in there;
     MicroPython (`logo_already_shown=true`) already has the centered C-boot logo (no fade).
   - When a partner band is shown, the logo then **slides up** to `logo_offset_y` (a delayed
     `lv_anim` after the fade). This is a deliberate divergence from Python (which fades the logo in
     already-raised) — it gives both platforms one motion and a seamless boot→splash handoff on the
     MCU (the held centered boot logo slides into place rather than jumping).
   - Phase machine (`lv_timer`, 50 ms): **INTRO** (fade+slide) → reveal version → **LOGO** (hold) →
     [reveal band → **PARTNER** (hold)] → emit completion. No-partner path holds `hold_final_ms`
     (~2 s) after the version before completing (so the version isn't shown-then-immediately-gone).
   - **Completion**: `seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE=1101,
     "splash_complete")` once. **Early dismiss** on touch/key when `dismissible` (default true).
   - `LV_EVENT_DELETE` cb frees the timer/group/ctx.

### JSON schema
`{ version:str, show_partner_logos:bool, sponsor_text:str (localized upstream),
logo_already_shown:bool, boot_logo_only:bool, dismissible:bool,
durations?:{fade_in_ms=1200, slide_in_ms=600, hold_logo_ms=1000, hold_final_ms=2000} }`.
`logo_already_shown`/`boot_logo_only` default false (Pi/desktop/screenshots fade in for parity).
The MicroPython call-site passes `logo_already_shown=true`.

### Wiring
- `seedsigner.h`: `void splash_screen(void*)` + `#define SEEDSIGNER_RET_SPLASH_COMPLETE 1101u`.
- Registries: `splash_screen` in `runner_core.cpp` and `screenshot_gen.cpp`.
- Scenarios: `tools/scenarios/scenarios.json` `splash_screen` block — base = CPython+partners +
  4 variations (`boot_logo`, `cpython_no_partners`, `micropython_partners`,
  `micropython_no_partners`) = **5** previews. Screenshots render each scenario's final still
  (fade/no-fade collapse to the same frame; the distinction is animation-only in the web runner).
- **Localized `sponsor_text`**: the localized catalogs (`tools/scenarios/localized/*.json`, generated
  + gitignored) are produced by `tools/i18n/gen_localized_scenarios.py`, which now translates **by
  content** — any string equal to a catalog msgid is translated (replaced the hardcoded
  key-allowlist), so `sponsor_text` (and any future field) localizes with no upkeep.
- `web_runner.cpp` / `screen_runner.cpp`: `SEEDSIGNER_RET_SPLASH_COMPLETE` added to the reserved-code
  classification.

### Verification (done)
- Screenshot generator: 4 resolutions × 5 scenarios; logo full-opacity, scales per resolution
  (pill 70/93/140), version + partner band laid out like the Python reference (HRF matches Python's
  size/position to ~2px). Zoom-crop crispness check at 70px clean (no white fringe / ringing).
  German render confirms localized `sponsor_text`.
- Builds: desktop tools at default (all heights); a restricted **single-height** build links with
  only its one variant (proves the gating); `screen_runner` links the full runner path.
- Web runner served for manual interaction.

---

## Part 2 — C-boot logo + binding + boot wiring (`seedsigner-micropython-builder`)
- **Submodule bump** `deps/seedsigner-lvgl-screens` to the Part-1 commit (gates the rest).
- **Binding** `mp_seedsigner_lvgl_splash_screen(cfg_dict)` in `bindings/modseedsigner_bindings.c`
  (dict-arg pattern, mirror `large_icon_status_screen`) + `MP_QSTR_splash_screen` in globals.
- **C-boot logo** in `display_manager.cpp` `seedsigner_board_startup()`: draw the **shared**
  `seedsigner_logo_img` centered on `lv_screen_active()` before the flush + backlight-on, holding
  the anti-white-flash sequence; offset the `SEEDSIGNER_DEBUG` label so it doesn't overlap.
- **Per-resolution firmware compile**: to honor "compile only the target resolution's assets," the
  builder should pass `SUPPORT_DISPLAY_HEIGHT_*` as CMake cache vars and gate the IDF font+image
  lists (today they're flat + rely on the downstream `-D` + link GC).
- **Handoff**: the held centered boot logo flows into the app splash, which (`logo_already_shown=true`)
  slides it up — no jump. Validate on-device that nothing repaints/clears the display between the
  boot logo and the `splash_screen()` call.

## Part 3 — App call-site (`seedsigner`)
Restore the splash at the `# TEMPORARY` marker in `controller.py` via `run_lvgl_screen(...,
allow_screensaver=False)`. Build cfg from settings: `version=f"v{self.VERSION}"`,
`show_partner_logos` from `SETTING__PARTNER_LOGOS`, `sponsor_text=_("With support from:")`,
`logo_already_shown=IS_MICROPYTHON`. `load_locale()` must run before passing localized
`sponsor_text`. (Upstream is the SeedSigner org → **kdmukai-only** commit trailer.)

## Open items / notes
- HRF is ~2px wider than Python at base — the new artwork is proportionally wider than Python's
  `hrf_logo.png`; unavoidable without distorting. Position/height match exactly.
- The unified center→up slide is an intentional divergence from current Python (which fades the logo
  in already-raised); documented in the screen code. Flip to Python-parity (fade already-raised on
  CPython, slide only on MCU) is a one-line gate if desired.
- Font `_150x` → `_133x` rename is a tracked follow-up (`TODO.md`), cross-repo.
- Dependency order to commit/PR: Part 1 → capture SHA → Part 2 (submodule bump) → Part 3.
