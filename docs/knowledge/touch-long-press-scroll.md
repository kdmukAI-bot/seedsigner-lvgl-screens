# Touch long-press-to-scroll button labels — the gesture, and the LVGL traps

**Scope:** `button_toggle_callback` + the `button_start_label_scroll` / `button_clip_label`
helpers in components.cpp. This is "Item 3" of `.claude/plans/pre-upstream-pr-hardening.md`:
the touch replacement for the per-focus marquee that hardware gets (A14). A button has no
persistent focus under touch, so a too-wide label can't marquee on focus the way it does on
hardware. Instead:

- **Short tap** (press → release): selects the button (unchanged).
- **Long-press-hold** (held past LVGL's ~400 ms threshold): scrolls the label so the user
  can read the full text — WITHOUT selecting.
- **Release after a long-press scroll**: does NOT select (the long-press consumed the
  gesture); the label clips back to its start.

This runs on the SAME callback the tap-to-select uses, so it covers every `button()` —
button lists and the main-menu grid. The hardware path (`button_set_label_marquee`, driven
from the nav layer's `update_visual_focus`) is unchanged and mutually exclusive: the pointer
indev is gated to touch mode (`runner_core.cpp`), and hardware "clicks" are synthesized with
`lv_obj_send_event(item, LV_EVENT_CLICKED, ...)` — a direct event send with no press cycle,
so they never produce a `LONG_PRESSED`. The two never run on the same screen.

## The four LVGL traps

### 1. LVGL sends `LV_EVENT_CLICKED` even AFTER a long press
The obvious implementation — "select on CLICKED" — does NOT skip the click just because a
long press happened. In `lv_indev.c` (pointer release, ~line 1525) only
`LV_EVENT_SHORT_CLICKED` is gated by `long_pr_sent`; `LV_EVENT_CLICKED` is sent on every
release where the press wasn't a scroll-gesture, long press or not. So a long-press-then-lift
still fires CLICKED and would select.

**Fix:** a "consumed" flag (`s_press_scrolled`). `LONG_PRESSED` sets it when it actually
starts a scroll; the touch branch of the CLICKED handler returns early (no select) when it's
set. (We can't switch selection to `SHORT_CLICKED` instead, because the hardware path sends
CLICKED directly and relies on it.)

### 2. `RELEASED` is the reliable "press ended" signal — not CLICKED
The label must clip back when the finger lifts. CLICKED is the wrong place: when a press turns
into a list scroll, LVGL sends NO CLICKED at all (the `scroll_obj == NULL` guard at release).
`RELEASED`, by contrast, is sent on every finger-up for an enabled pressed object, and it
fires BEFORE the CLICKED that may follow. So the label is restored on RELEASED; CLICKED only
suppresses the selection (the label is already clipped by then). `PRESS_LOST` is handled as a
backstop for the press-taken-over case. The consumed flag is cleared on the NEXT `PRESSED`
(and after the suppressed CLICKED), so a stale flag never leaks into the next gesture.

### 3. Suppress the select ONLY when something actually scrolled
`button_start_label_scroll` returns whether a scroll really started — i.e. the label
overflows AND the locale is in scope — and only then is the press consumed. A long-press on a
label that FITS (or an out-of-scope RTL locale) leaves the press unconsumed, so it still
selects on release. This matters on a signing device: a deliberate, firm press on a normal
button shouldn't silently fail to select just because it crossed 400 ms. "Hold to peek" only
overrides "tap to select" when there's actually something to peek at.

### 4. Overflow detection differs for subset vs shaped labels
The "does it overflow" test can't be one-size-fits-all:
- **Subset/Latin:** measure the label's STORED (presentation-form) text with
  `lv_text_get_size` vs the content box — same test `apply_button_label_layout` uses.
- **Shaped (hi/th):** the codepoint measure mis-counts the on-screen presentation forms /
  conjuncts, so ask the baked glyph run instead. New accessor
  `seedsigner_label_run_overflows()` (glyph_runs.cpp) reports `run->layout_w > content_w` —
  the exact comparison `glyph_run_draw_cb` makes — returning -1 when no run is attached so the
  caller falls through to the codepoint measure.

## Reusing the auto-scroll helper — two independent holds
The scroll itself is `label_set_line_autoscroll()` (see `label-autoscroll-feel.md`), the same
helper the title/headline use, so the touch scroll gets the true ~40 px/sec rate (NOT LVGL's
default speed, which the user flagged as too fast). Item 3 split its single hold parameter
into two:
- `begin_hold_ms` — the initial pause before scrolling. Touch passes **0**: the long-press is
  itself the pause, and the label clips back on release, so an initial hold would hide the
  motion behind a quick release.
- `loop_hold_ms` — the pause each time the line wraps back to the start. Touch keeps
  `LINE_SCROLL_BEGIN_HOLD_MS` so it still gets a beat at the start each loop (the user's
  request after first review).

The title/headline pass `LINE_SCROLL_BEGIN_HOLD_MS` for both (unchanged). Mechanically, the
template anim's `act_time = -begin_hold_ms` and `repeat_delay = loop_hold_ms` are now fed
independently; `begin_hold = 0` leaves `act_time` at 0 (immediate) while a non-zero
`repeat_delay` still lands a hold at the wrap-to-start reset.

## RTL (fa/ur) is excluded
Matching Task 0 / `glyph_run_draw_cb` (offset, scroll-mode start-justify, content-box clip are
all LTR-only for now) and the hardware marquee / title / headline scroll. `button_start_label_
scroll` early-returns for `seedsigner_locale_is_rtl()`, so an overflowing fa/ur button label
just stays clipped and selects normally on a long press. RTL button scroll lands with the ur
RTL track.

## Why the static screenshot gallery stayed byte-identical
The screenshot generator renders headless with no pointer events, so `LONG_PRESSED` never
fires and none of this code runs at capture time. The autoscroll-signature change is
value-preserving for the existing callers. Verified: 82 PNGs × 7 locales (en/de/el/fa/hi/th/ur)
byte-identical to the pre-change tip (only the `manifest.json` timestamp differs).
