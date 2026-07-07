# Glyph-run word-wrap collapsed complex-script single-line labels to their ASCII prefix

## Symptom
On a `button_list` screen in a shaping locale, a complex-script **top_nav title**
rendered only its leading ASCII characters and silently dropped the script text.
Concretely, the Thai title `1 อินพุต` (msgid `1 input`) rendered as just `1`, while
the **identical string rendered in full as a button label** on the same screen.
Devanagari (`hi`) and Urdu (`ur`) titles looked fine, so it presented as
Thai-specific — but it was not really about Thai.

## Root cause
The glyph-run render pass (`glyph_runs.cpp::attach_runs`) word-wraps a matched run
to the label's content width:

```cpp
int wrap_width = g_table.rtl ? 0 : lv_obj_get_content_width(obj);
run = bake_run(..., wrap_width);
```

`bake_run` → `wrap_line` splits the shaped run at its offline ICU break marks
whenever it overflows `wrap_width`. That is correct for genuinely wrapping body
text, but it was applied to **every** LTR label regardless of long mode.

The `top_nav` title is `LV_LABEL_LONG_SCROLL_CIRCULAR` — a **single-line** mode —
and its content box is the narrow region clipped between the back/power buttons
(`components.cpp::top_nav`). When the shaped run is wider than that narrow region,
`wrap_line` cut it at the space break into two visual lines (`1` / `อินพุต`). The
baked mask was then two lines tall, but a single-line label only shows line 0 — so
the script text on line 1 was painted below the title and never seen.

Why it looked Thai-specific:
- It only triggers when the run **overflows the title region**. The Thai run was
  wide enough to overflow; the Devanagari `1 इनपुट` run happened to fit, so `hi`
  never wrapped. `ur` is RTL, and the RTL path already passed `wrap_width = 0`.
- So the bug is "single-line label whose run overflows its box," not "Thai." Any
  script could hit it with a long enough title.

This is independent of the host platform — it reproduces identically on the desktop
native build and on the Pi Zero (ARMv6) build, because it lives entirely in the
shared screen layer. It surfaced during the first on-device i18n render test.

## Fix
Only word-wrap labels that actually wrap their codepoint text — i.e.
`LV_LABEL_LONG_WRAP`. Every other long mode (`SCROLL`, `SCROLL_CIRCULAR`, `CLIP`,
`DOT`) is single-line and must keep the run on one line:

```cpp
const bool wraps =
    (lv_label_get_long_mode(obj) == LV_LABEL_LONG_WRAP) && !g_table.rtl;
const int wrap_width = wraps ? lv_obj_get_content_width(obj) : 0;
```

`wrap_width` is then passed to both `bake_run` and `bake_segmented`. This mirrors
LVGL's own behavior: a `SCROLL_CIRCULAR`/`CLIP` label never wraps its plain text
either — it scrolls or clips a single line — so the glyph-run path should not wrap
it. The intended consumer of `LV_LABEL_LONG_WRAP` is the multi-line body text in
`seedsigner.cpp`; that path is unchanged. But note the corollary in the recurring
gotcha below: a label that *should* be single-line but is left in the default WRAP
mode will be wrapped by this path too.

A too-long shaped single-line title now clips at the box edge instead of collapsing
to its ASCII prefix. (It does not scroll-animate — the run mask is static and the
label's own scrolling text is suppressed — but the text is at least present and
correct, matching how the plain-codepoint path degrades.)

## Validation
- Dev-box native build: `th` title renders full; `hi`/`ur` unchanged (no regression).
- Pi Zero (ARMv6) build: same — confirmed on hardware via framebuffer capture.
- The identical string as title vs. button (the original discriminator) now renders
  identically.

## Recurring consumer-side gotcha: new single-line body labels default to WRAP

The engine fix above is correct, but it shifts the responsibility onto the *screen
author*: `lv_label_create()` produces a label whose long mode is
`LV_LABEL_LONG_WRAP` by default. Any newly added label that is meant to hold a
**single line of body text** but is left at that default will have its shaped run
wrapped to the label's content width — and if that box is narrow, a long
translation (Thai is the usual first offender because its strings are wide and it
ships early) breaks onto line 2, which a one-line-tall label never shows. It looks
fine in English and in the widest display profile (where the box is roomy) and
collapses only in a shaping locale at the 240-wide profile, so it slips through
casual review.

This has now bitten twice, both on recently added PSBT screens:

1. **`psbt_math_screen`** — the info word (e.g. Thai for "fee") wrapped onto the
   rule-off line below it. Fixed by forcing `LV_LABEL_LONG_CLIP` **and** an explicit
   width equal to the room to its right (`seedsigner.cpp`, `add_info` lambda).
2. **`psbt_change_details_screen`** — the "Address verified!" confirmation
   (`vtx`, Thai `ตรวจสอบที่อยู่แล้ว!`) lives in a tight `LV_SIZE_CONTENT` flex row and
   collapsed to just its first word `ตรวจสอบ` at 240×240, while 800×480 showed it in
   full. Fixed by forcing `LV_LABEL_LONG_CLIP` on `vtx` (no explicit width — the row
   must stay content-sized so the icon+text unit still measures and centers as a
   tight block).

A codebase audit (2026-07-06, after the second instance) swept every
`lv_label_create` in `seedsigner.cpp`/`components.cpp` for the pattern. Two more
labels shared the *exact* tight-container structure and were guarded with
`LV_LABEL_LONG_CLIP` proactively (both still render byte-identically today — their
current translations fit — so the guard only matters for a future longer string):
- **`seed_finalize_screen`** fingerprint label (the gray "fingerprint" caption in the
  IconTextLine col/row).
- **`qr_display_screen`** brightness hint rows (`qr_build_hint_row`, hardware mode —
  the "Brighter"/"Darker" text beside each chevron).

Lower-risk single-line body labels also exist but are NOT in a tight container — they
sit on a full-width / full-screen parent and are positioned by `lv_obj_align` or
absolute `set_pos` with `LV_SIZE_CONTENT` width, so LVGL never clamps them and the run
never wraps for the current locales (splash sponsor text, loading-screen status text,
psbt_overview chart labels). They were left as-is; a translation wider than the whole
screen would overflow horizontally there rather than collapse, a different failure.

The rule of thumb that predicts the collapse: it fires only when the label's natural
single-line width (with its siblings, e.g. an icon) can **exceed the container's max
width** so `LV_SIZE_CONTENT` gets clamped — which is why it shows up first on the
240-wide profile and in shaping locales (widest strings), and why English / the
800-wide profile look fine.

**Authoring rule:** a body-font label that is conceptually one line must be set to
`LV_LABEL_LONG_CLIP` (or a SCROLL mode if it should marquee). Only reach for the
default `LV_LABEL_LONG_WRAP` when the label is genuinely multi-line and its box has a
real, generous width. Two variants of the CLIP fix:
- **Fixed region** (label sits in a known-width slot): also `lv_obj_set_width()` to
  that slot so it clips gracefully at the edge instead of at its codepoint width.
- **Tight/centered unit** (label is content-sized inside an `LV_SIZE_CONTENT` row
  that is measured and positioned afterward): CLIP only, no explicit width — an
  explicit width would break the content-sizing the centering math depends on. This
  relies on the shaped run being no wider than the codepoint box (true for Thai,
  whose marks stack to zero advance); if a future script shapes *wider* than its
  codepoints the run could clip, in which case measure the run width explicitly.

See also `complex-script-run-pipeline.md` (the offline shaping + on-device run model).
