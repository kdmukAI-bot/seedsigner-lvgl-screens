# button_list focus under overflow — the scroll-then-buttons trap

Two non-obvious couplings around `button_list_screen` focus, both surfaced on-device (Pi
Zero) during the `seedsigner`-side settings-multiselect / Tier-A migrations. The fixes
shipped in PR #38 (`681451b`); this note captures *why* they were needed so the traps
aren't re-hit. Anchors: `components/seedsigner/navigation.cpp` (`nav_bind`),
`components/seedsigner/seedsigner.cpp` (`bind_screen_navigation`, `make_body_text_label`).

## The invariant: default-index value carries focus intent

`bind_screen_navigation` auto-enables **scroll-then-buttons** mode for a vertical screen
with non-focusable intro content (`upper_body != body`) whose body **overflows** the
viewport. In that mode `nav_bind` sets `NAV_ZONE_SCROLL` and jumps the list to the top
(`lv_obj_scroll_to_y(..., LV_ANIM_OFF)`) — so nothing is highlighted and the first DOWN
scrolls. Correct for read-heavy screens (long warnings/notices); wrong if a caller asked
to restore focus.

The unified rule that resolves this is carried by `nav_config_t.initial_body_index`:

- **Concrete index → stays focused even when the body overflows** (scrolled into view).
  Button lists / menus pass **0**; an explicit `initial_selected_index` (a focus-restore
  on re-render) flows through the same path. So a button list always has a selection.
- **`NAV_INDEX_NONE` → "no forced default"**: the first item is focused when the screen
  **fits**, but the screen starts **unfocused (read-first)** while scrolling is required.
  Status / warning screens pass this.

`nav_bind`'s overflow branch keeps focus iff `initial_body_index != NAV_INDEX_NONE`. (The
index value alone expresses intent — this replaced an earlier separate
`initial_index_explicit` flag.)

## Trap 1 — explicit focus-restore dropped on in-place re-render

A settings **multiselect** (`SettingsEntryUpdateSelectionView`) re-renders the whole
screen on every toggle, forwarding `initial_selected_index = <toggled row>` to restore
focus. Each rebuild re-triggered scroll-then-buttons, whose top-jump **overrode** the
just-computed restore index → the list bounced to the top with nothing highlighted every
toggle. The PIL screen never showed this (it re-highlighted `selected_button` each
render); the native path (scroll-then-buttons + explicit restore index + repeated
re-render) was simply never exercised until the migration.

Resolved by the invariant above: a concrete restore index survives the override. **Cross-repo
contract:** the `seedsigner` side forwards a **non-zero** `selected_button →
initial_selected_index`; index 0 is covered by the concrete-0 default, so the existing
`if selected_button:` truthiness guard (which drops 0) is harmless and needs no
`restore_selected_index` distinct-signal key.

## Trap 2 — loose intro-text leading silently changes focus behavior

The intro `text` above a button list and the `large_icon_status_screen` body both build
via the shared `make_body_text_label` with the **same `BODY_FONT`** (identical glyph
size), but diverged on the line-spacing post-step:

- status body applied ink-based `tight_line_space(...)` + `margin_top = 0` ("matches the
  PIL reference");
- button_list intro text stopped after `make_body_text_label`, inheriting the **loose
  screen-wide `BODY_LINE_SPACING`**.

The taller loose block tips a short prompt into **marginal** overflow, which trips
scroll-then-buttons → **no button highlighted on load**. The trap: a purely *cosmetic*
line-spacing difference silently flips *focus behavior*, because overflow is the trigger.
Fixed by factoring the tight-spacing step into a shared `apply_body_tight_line_spacing()`
helper called by **both** call sites.

## Not a bug: intro text "looks lighter" than a PIL warning body

Pre-cutover on CPython, warning/large-icon screens are still **PIL** while the migrated
button list is native Tiny-TTF. PIL `TextArea` **supersamples** body text below 20 px
(`supersampling_factor = 2`), so its 17 px body reads heavier/crisper. This is *two
rendering engines*, not a native font/spacing defect — it disappears at the PIL→LVGL
cutover, and the broader "native small-text strength" concern is tracked in
[`../font-low-dpi-rendering-plan.md`](../font-low-dpi-rendering-plan.md).

## Related
- [`button-list-initial-selection-scroll-animation.md`](button-list-initial-selection-scroll-animation.md)
  — the restored selection must jump (`LV_ANIM_OFF`) on the *initial* render, not animate;
  it sits on top of the focus-restore behavior above.
