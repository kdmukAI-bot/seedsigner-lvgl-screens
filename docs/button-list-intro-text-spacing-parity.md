# button_list_screen — intro `text` line spacing (loose leading → overflow)

_Status: **line-spacing bug FIXED (2026-06-26)**; the **text-strength forward note** remains (that's
the supersampling item — see `font-low-dpi-rendering-plan.md`). Surfaced on-device (Pi Zero) during
the `seedsigner`-side Tier-A migration (`SeedTranscribeSeedQRConfirmQRPromptView`), 2026-06-25. The
seedsigner side passes the right `text` + `is_bottom_list`; the gap was native._

> **Fix as built:** the intro-text post-step was factored into a shared
> `apply_body_tight_line_spacing()` helper (`seedsigner.cpp`), now called by **both** the
> `large_icon_status_screen` body and the `button_list_screen` intro text — so the intro block uses
> the same tight, ink-based `tight_line_space` (+ `margin_top=0`) as the status body instead of the
> screen-wide loose `BODY_LINE_SPACING`. The status-screen call site is byte-identical (same helper).
> Verified headlessly via `button_list_screen / intro_text_marginal` (before/after in
> `review/button-list-fixes/C_BEFORE_loose_marginal.png` vs `C_AFTER_tight_marginal.png`): a 3-line
> prompt + 3 buttons that overflowed (no button highlighted) with loose spacing now fits and
> highlights the default button. The "text looks weaker" half of this doc is unrelated and tracked
> as the supersampling effort.

## The real native bug: loose intro-text leading → marginal overflow

The optional intro text above a `button_list_screen`'s buttons renders with **looser inter-line spacing**
than the native `large_icon_status_screen` body. The taller block tips a short prompt into a **marginal
overflow** ("slightly scrolls"), which trips **scroll-then-buttons** mode so **no button is highlighted on
load** (see `button-list-focus-restore-scroll-then-buttons.md`).

Root cause — both screens build text via the shared `make_body_text_label` (`seedsigner.cpp` ~700) with the
**same `&BODY_FONT`** (glyph size is identical), but diverge on the line-spacing post-step:

- **status-screen body** (`seedsigner.cpp` ~1205–1227): applies `tight_line_space(&BODY_FONT, <text>,
  LIST_ITEM_PADDING/2)` (ink-based advance, "matches the PIL reference") + `margin_top = 0`.
- **button_list intro text** (`seedsigner.cpp` ~736–742): calls `make_body_text_label` and stops — inherits
  the **loose screen-wide `BODY_LINE_SPACING`** (`seedsigner.cpp` ~448). The comment even says so: *"line
  spacing is the screen's inherited default."*

**Fix:** give the intro-text label the same tight-spacing (+ `margin_top=0`) treatment the status body gets —
ideally factor that post-`make_body_text_label` step into a shared helper so both call sites are
byte-identical. This shrinks the block out of overflow → scroll-then-buttons no longer activates → the
default button is highlighted on load. This is the primary lever for the **first-render** no-default-button
case; the **multiselect re-render** focus loss is the separate fix in
`button-list-focus-restore-scroll-then-buttons.md`.

## NOT this bug: the "text looks weaker/smaller" is PIL supersampling (transitional)

On-device the intro text also *reads as lighter/smaller* than the warning screen's body — but that is **not**
font size or line spacing. On CPython (Pi) the warning/large-icon screens are **still PIL**
(`view.py` `WarningScreen`, non-MicroPython branch; native `large_icon_status_screen` runs only under
`IS_MICROPYTHON`). PIL `TextArea` **supersamples** body text **below 20px** (`supersampling_factor = 2`,
render at 2× then downscale) — and the 240-profile body font is 17px, so warning body text is supersampled,
giving heavier/crisper strokes. The migrated `button_list_screen` is native Tiny-TTF (no supersampling). So
the comparison is **two rendering engines**, not a native font/spacing defect — it disappears at the
PIL→LVGL cutover when the warning screen is also native.

## Forward concern (font effort): native text strength at cutover

The above implies a real, broader item: PIL's supersampling of sub-20px text is what makes the current UI's
small text look "strong." When PIL is removed, **all** text becomes native Tiny-TTF, so the whole UI loses
that — unless native rendering (AA depth / glyph `bpp`, or font weight) clears the bar PIL set. Worth the
font work measuring native Tiny-TTF small-text rendering against the PIL supersampled reference before the
cutover, so the all-native build doesn't read as uniformly lighter than today's.

## References

- `components/seedsigner/seedsigner.cpp` — `make_body_text_label` (~700); button_list intro text call
  (~736–742); screen-wide loose line space (~448); status-screen body tight line space (~1205–1227).
- `seedsigner` repo: `gui/components.py` `TextArea.supersampling_factor` (=2 default; disabled ≥20px;
  body 17px ⇒ supersampled); `views/view.py` — `WarningScreen` (PIL) on CPython vs native
  `large_icon_status_screen` only under MicroPython; `SeedTranscribeSeedQRConfirmQRPromptView`
  (`views/seed_views.py`) — the migrated screen that surfaced this.
- Related: `docs/button-list-focus-restore-scroll-then-buttons.md`.
