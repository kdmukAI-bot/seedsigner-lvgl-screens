# lv_shape — Arabic/Persian shaping oracle

A tiny standalone tool that links the repo's `third_party/lvgl` and runs LVGL's
own Arabic/Persian shaper over text, so the font-pack builder can subset a font
to exactly the glyphs the renderer will request.

## What it does

Reads UTF-8 text on **stdin**, runs it through LVGL's `lv_text_ap_proc`
(`LV_USE_ARABIC_PERSIAN_CHARS=1`), and prints the unique set of output code
points as a **JSON array of integers** on stdout.

```bash
printf 'بیت‌کوین' | tools/i18n/shaper/build/lv_shape
# [8204,64400,64510,64511,65169,65174,65254,65262]
```

That output is exactly the set of code points the renderer will look up when it
draws the same text — i.e. the cursive **presentation forms** the base letters
resolve to, plus any pass-through code points (digits, punctuation, ZWNJ).

## Why it exists (and why standalone)

Arabic/Persian is cursive: each letter takes an isolated / initial / medial /
final form depending on its neighbors. LVGL's "basic" shaper rewrites the logical
base letters into presentation-form code points (U+FB50–FDFF, U+FE70–FEFF) *before*
glyph lookup, so a subset font only needs those forms, not the base letters.

The font-pack builder (`../build_fontpacks.py`) needs to know which forms a
locale's corpus actually produces. Rather than re-implement the shaper in Python
(and risk drift), `lv_shape` runs the **real** LVGL code. That gets the tricky
cases right for free:

- The buggy `ap_chars_map` digit row `{0x06F0,-1,2,0}` only ever emits its
  isolated form in real contexts (a naive "all four forms" expansion would pull
  in unrelated glyphs).
- ZWNJ (U+200C) is preserved as a join-breaker.
- `لا` collapses to the lam-alef ligature.

It's a **separate binary**, not a subcommand of the screenshot generator, because
the language packs are consumed by every renderer in the ecosystem (Pi Zero
CPython extension, ESP32 MicroPython firmware, desktop tools). Pack preparation is
a shared build-time step, so its shaping oracle is its own tool. It links the same
`third_party/lvgl` all consumers share, so the forms it emits match what each
consumer's renderer will request, and is built with `LV_USE_ARABIC_PERSIAN_CHARS=1`
+ `LV_USE_BIDI=1` to mirror the render-time LVGL config.

Policy lives in the caller: `lv_shape` emits **all** output code points (ASCII
included). `build_fontpacks.py` applies the ASCII-exclusion policy (the baked-in
OpenSans floor covers ASCII) and does the actual subsetting.

## Build

`build_fontpacks.py` builds it on demand (pass `--shaper-bin` to point at an
existing binary). To build it manually:

```bash
cmake -S tools/i18n/shaper -B tools/i18n/shaper/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/i18n/shaper/build -j
```

## Implementation notes

- `lv_text_ap.h` is a private LVGL header (not in the `lvgl.h` umbrella), so
  `lv_shape.c` includes `src/misc/lv_text_ap.h` directly.
- `lv_init()` is called so the shaper's `lv_malloc`/`lv_free` have an allocator.
  It is headless and silent (`LV_USE_LOG` defaults to 0 under `LV_CONF_SKIP`), so
  nothing is written to stdout to corrupt the JSON.
- Uniqueness + ordering use a flat presence bitmap over the Unicode range — the
  simplest correct approach for a one-shot CLI.
