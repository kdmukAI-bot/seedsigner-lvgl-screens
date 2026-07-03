# Body-text rendering on the Pi Zero 240×240 panel — root cause + fix options

_Status: **investigated; root cause revised; fix recommended.** Resumable design doc
for a fresh session. Discovered 2026-06-26 while eyeballing the LVGL build on real Pi Zero hardware.
**2026-06-25 revision:** root cause corrected — PIL's crispness comes mostly from a deliberate
**supersample + SHARPEN** pipeline (body-only), not from hinting alone; recommendation re-ordered to
**option B (supersampling)**. LVGL feasibility assessed (feasible, Pi-only). The implementation plan
for option B is now the "Implementation plan — option B" section below (previously a separate
`font-low-dpi-supersampling-plan.md`, folded in here). **Not yet on `main`:** an A/B evaluation of the
*per-label* variant (panel-gamma sim + body-only SSAA as web-runner / screenshot-gen toggles) is built
on branch `feat/pizero-body-supersampling` (`bcf39dd`); the production Pi-flush path is still unbuilt._

> **This is a SEPARATE thread from the font-memory dedup work (PR #37 / `feat/font-memory-dedup`).**
> That PR is a byte-identical memory optimization and has nothing to do with this rendering issue.
> Do not conflate them. This doc stands alone.

## Symptom

On the **Pi Zero Waveshare 240×240 ST7789** panel, LVGL **body text** renders thin / fuzzy / light
with loose line spacing — visibly worse than the PIL reference **on the same panel**.

- Even **SemiBold** roles (titles, button labels) are softer than PIL's buttons; **body (Regular)** is
  the worst. So *every* weight is affected, not just Regular.
- **240×240 screenshots look fine** — but only because they're viewed *zoomed* on a monitor.
- **ESP32-P4 looks fine** — because it's a higher-resolution panel (more pixels per glyph).

Evidence photos (same panel, two screens):
- `review/pizero-body-text/PXL_20260626_015155523.RAW-01.COVER.jpg` — **PIL** ("Caution / Privacy
  Leak!" / "Xpub can be used to view all future transactions.") — crisp, even, solid strokes.
- `review/pizero-body-text/PXL_20260626_015051315.RAW-01.COVER.jpg` — **LVGL** ("Confirm SeedQR?" /
  "Optionally scan your transcribed SeedQR…") — thin, fuzzy, looser line spacing.

## Root cause (confirmed)

> **Correction (this revision).** An earlier version of this doc blamed the gap solely on
> **missing FreeType hinting**. That is only part of the story — and not the dominant part for the
> worst-affected role. The bigger factor is that **PIL deliberately supersamples body text** and
> LVGL does not. See "How Python actually renders crisp text" below. Both mechanisms are real, but
> they hit *different roles*.

It is the **rasterizer pipeline + physical pixel pitch**, NOT the display color pipeline and NOT a
subpixel/color-layout effect:

- Both the screenshot generator (`screenshot_gen.cpp` → `LV_COLOR_FORMAT_RGB565`) and the Pi Zero
  backend render at **RGB565** → the framebuffer **bytes are identical**. The screenshot is not
  "better data." (`seedsigner-raspi-lvgl/docs/lvgl-v9-migration-plan.md` confirms the Pi path is
  RGB565, 2 bytes/px.)
- **Primary, for the body role: PIL supersamples; LVGL renders at native 1×.** PIL renders body
  text at **2× size into an offscreen buffer, downscales it with LANCZOS, and applies a SHARPEN
  unsharp pass** before compositing (see the next subsection). LVGL's `lv_tiny_ttf` rasterizes the
  glyph once, directly at 17 px, with no oversample and no sharpen. This is why **body (Regular) is
  the worst-affected role** — it is precisely the role PIL supersamples and LVGL does not.
- **Secondary, for titles/buttons: missing hinting.** `lv_tiny_ttf` = stb_truetype does **NO
  hinting** (pure grayscale AA on the raw outline). PIL — and the *old baked `lv_font_conv` `.c`
  fonts* — use **FreeType, which hints** (grid-fits and effectively darkens stems). Crucially, PIL
  does **not** supersample titles (20 px) or buttons (18 px) — so for those roles the only
  difference is hinting, which is why they read as "softer than PIL" but less dramatically than
  body. The Stage-A baked→tiny_ttf switch (`docs/font-tiering-plan.md`) is when the hinting
  regressed.
- **Both effects shrink as pixels-per-glyph grows.** At 17 px body on a ~1.3″ 240×240 panel,
  unhinted/un-supersampled stems land on fractional pixels and smear at partial coverage →
  thin/light/fuzzy. Give the same outline more pixels (the P4's larger/denser panel) and both the
  missing supersample and the missing hinting stop mattering → P4 looks fine.
- Why screenshots deceive: a 240×240 PNG viewed zoomed on a monitor reads soft AA as smooth; at the
  panel's native pitch, next to crisp PIL, the same pixels read as fuzzy.

This is the open risk `font-tiering-plan.md` already flagged ("TTF-vs-baked metric parity… wraps/
spaces a hair differently"). The panel just makes it obvious. See also
`docs/knowledge/font-loading-binfont-vs-tiny-ttf.md`, `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.

## How Python actually renders crisp text

The crispness comes mostly from a deliberate **supersampling + sharpen** pipeline in
`TextArea`, not from hinting. Source of truth: `seedsigner/src/seedsigner/gui/components.py`.

1. **Render at 2× the target px.** `supersampling_factor = 2` by default; the font is loaded at
   `2 * font_size` (body 17 px → **34 px**). — `components.py:149`, `components.py:287`
2. **Draw all lines into a 2×-sized RGBA temp canvas**, with a 10 px `resample_padding` top/bottom
   to avoid edge dimming. — `components.py:276`, `components.py:292-327`
3. **Downscale to target size with LANCZOS** — `components.py:337`
4. **Apply `ImageFilter.SHARPEN`** — PIL's fixed 3×3 unsharp kernel (center **+32**, eight
   neighbors **−2**, divisor **16**). **This is the "strengthen" step** that turns smooth-but-light
   AA into solid strokes. — `components.py:338`
5. **Crop the padding** and composite. — `components.py:340`

**It is applied per-role, and only where it helps:**

| Role | Size (240 panel) | Supersampled in PIL? |
|---|---|---|
| **Body** | 17 px | **Yes — 2× + LANCZOS + SHARPEN** (`font_size < 20`) |
| Top-nav title | 20 px | No — auto-disabled at `font_size >= 20` (`components.py:266-268`) |
| Button label | 18 px | No — explicit `supersampling_factor=1` (`components.py:1275`, *"black text on orange supersamples poorly"*) |

So when PIL supersamples body it rasterizes the glyph at **34 px** — a size where hinting barely
matters — and gets its crispness from the **downscale + SHARPEN**, not from grid-fitting a 17 px
glyph. **Implication:** lv_freetype/hinting alone (option C) would *not* reproduce PIL body text;
the script-agnostic match for body is **supersampling done the way PIL does it, including the
SHARPEN pass** (option B). Hinting is the right fix for the *non-supersampled* roles
(titles/buttons), where PIL relies on it too.

## The tools flatter the rendering — panel intensity falloff (and a proposed contrast sim)

A second, independent reason the screenshots and web runner mislead — beyond "viewed zoomed":
**the Pi Zero ST7789 panel has a far more extreme intensity falloff between bright and dim pixels
than a monitor does.** The anti-aliasing math is the *same* on both — the framebuffer bytes are
identical (proven above) — but the panel's response curve is not the monitor's:

- On a monitor, an anti-aliased edge pixel at, say, 40–60% gray reads as a smooth, *effective*
  blend that visually thickens the stroke. The AA "does its job."
- On the panel, that same mid-gray value is reproduced **much dimmer relative to the full-bright
  pixels** — a steep intensity drop-off, not a gentle ramp. So the partial-coverage edge pixels
  that carry a thin stroke **fade toward the background** instead of reinforcing the stroke. Thin
  text that looks fine in the tools reads as faint and hard to make out on the device.

This compounds the supersampling gap: PIL's body text survives the panel because the supersample +
SHARPEN pushes edge pixels *brighter/denser* (more full-coverage, fewer faint partials), so there
is less for the panel's falloff to swallow. Native Tiny-TTF body text is mostly faint partials at
17 px → the panel's curve eats it.

**Proposed tooling — a panel-response (contrast/gamma) simulation, so we can iterate without
hardware.** Add a "Pi Zero panel" toggle/slider to the **web runner** (and an equivalent
`--panel-gamma` flag to the **screenshot generator**) that applies the panel's approximate
intensity transfer curve to the rendered framebuffer before display/PNG:

- Model it as a gamma / contrast curve (a single γ exponent to start, refined toward a measured
  curve later — ideally sampled from a photo of the panel showing a gray ramp). A slider lets us
  dial it to match what the eye sees on-device.
- **Why it matters for this whole effort:** without it, the tools will make *every* candidate fix
  (B supersampling, C tight spacing, lv_freetype) look "good enough," because the monitor hides the
  falloff. With it, we can (1) **reproduce the readability problem in-tool**, then (2) **evaluate
  how much the supersampling actually recovers** — turning the hardware round-trip into an
  occasional confirmation rather than the only way to judge a change.
- Locus: cheapest in the desktop/web display path (a post-render LUT over the RGB565/RGBA buffer),
  exactly where the real Pi flush would also live — it does not touch the shared screen code.

This is a **prerequisite for trustworthy headless evaluation of option B**; see the
"Implementation plan — option B" section below.

## Scope

- Affects the **240-height profile only** → in practice the **Pi Zero** (the only 240×240 target).
  ESP32-S3 / P4 higher-res panels are fine.
- Affects **all locales/scripts** — it's below the typeface, in rasterization. **Any complete fix must
  be script-agnostic.** (This is why the weight-bump option, below, is only a partial fix.)

## Options

### A. Weight bump at 240 (Latin tactical, fast)
Bump every role **+1 weight step at the 240 profile**: the four SemiBold roles (title, main-menu,
large_button, button) → **Bold**; body (Regular) → **SemiBold**. This manually approximates what
hinting's stem-darkening does.
- **Needs:** bake an **OpenSans Bold (Western block)** subset (we have Regular + SemiBold; same
  pipeline that produced those — `tools/i18n/build_fontpacks.py` block-range mode + bin2c into
  `components/seedsigner/fonts/`).
- **Where:** `gui_constants.cpp::install_western_baseline()`, gated on `PX_MULTIPLIER_100` — mirrors
  the existing `large_button` 20/18 per-profile quirk.
- **Limits:**
  - **Latin-ONLY.** The Noto CJK/Devanagari/Arabic/Thai packs ship a *single Regular weight* — there
    is nothing to bump to. Non-Latin locales stay thin at 240 unless we also ship heavier Noto subsets
    (pack bloat) or fix it at the rasterizer.
  - **Tactical stand-in for hinting.** If lv_freetype (option C) lands later, a hand-bolded weight is
    *over*-corrected → must be reverted then.
- **Risk:** low (known baked-bytes path). Good for a fast, visible win on the default (Latin) UI.

### B. Supersampling at 240 — **this is literally what PIL does** (uniform, Pi-only)
Render at higher resolution, downsample, **then sharpen** — exactly PIL's pipeline (see "How Python
actually renders crisp text"). Uniform across all scripts, Pi-scoped (240 only runs on the Pi, which
has headroom the ESP32 lacks). **This is now the recommended primary fix** — see the LVGL
feasibility section below for the full design and the "Implementation plan — option B" section for
the concrete steps.

- **The SHARPEN pass is not optional.** PIL pairs the LANCZOS downscale with `ImageFilter.SHARPEN`
  (3×3 unsharp). An earlier version of this option warned that supersampling "smooths AA but stays
  light" — that caveat exists **only because it omitted the sharpen step**. Downscale + SHARPEN is
  what produces PIL's *strong* strokes, not just smooth ones. Any port must include it; LVGL has no
  sharpen primitive, so it's ~30 lines hand-rolled on the downscaled buffer.
- **Whole-screen 2× SSAA (recommended locus):** render the existing **480 profile**
  (`PX_MULTIPLIER_200`, an exact geometric 2× of the 240 profile) and box-downscale to 240×240 in
  the **raspi-lvgl flush**, then sharpen. Lives almost entirely in the Pi backend; also crisps
  icons/shapes; **uniform across every role, weight, and script**. Cost: 4× render →
  **marquee/scroll/screensaver animations heavier on ARMv6** (the one thing a spike must measure).
  Needs a square **480×480** profile — `make_profile(480,480)` is a one-line additive change
  (current profiles are 240×240/320×240/480×320/800×480; `480 = 2×240` is the clean 2× we need).
- **Per-label glyph oversampling (faithful-to-PIL fallback):** oversample only the **body** label
  (as PIL does — titles/buttons are left native), via `lv_canvas` + `lv_draw_label` at 2×,
  bilinear-downscale, hand-rolled sharpen (cached → ~zero per-frame cost). More faithful and cheaper
  per frame, but invasive in shared `components.cpp` and conflicts with body marquee/scroll/wrap and
  the pre-shaped `glyph_runs.cpp` path (hi/th/ur). `lv_tiny_ttf` exposes no oversampling setting, so
  this needs the canvas route or a vendored-lib patch.
- **Downscale quality:** LVGL's image transform is **bilinear at best (no LANCZOS)**, but for an
  integer 2:1 reduction a 2×2 **box average ≈ LANCZOS** — fine here.

### C. lv_freetype on the Pi path (durable; complement, not the headline)
Swap the runtime rasterizer from `lv_tiny_ttf` to **`lv_freetype` on the Pi only** (font_registry
gains a compile-time rasterizer selector; **ESP32 keeps tiny_ttf** where memory is tight and the
higher-res panels already look fine).
- FreeType **is PIL's engine** → **true hinting** → the right fix for the **non-supersampled roles**
  (titles, buttons), where PIL also relies on hinting, with **no extra font assets**.
- **But it does not reproduce PIL's body text on its own.** PIL gets body crispness from the 2×
  supersample + SHARPEN (rasterizing at 34 px, where hinting barely matters), not from hinting a
  17 px glyph. So C closes the titles/buttons gap but leaves body short of PIL unless paired with B.
- **Verified present:** `lv_freetype` ships in our LVGL v9.5.0 pin
  (`third_party/lvgl/src/libs/freetype/`); **libfreetype 26.2.20** is on the Pi build host;
  `seedsigner-raspi-lvgl` already references FreeType in its i18n oracle tooling.
- **Open spike risk:** confirm `lv_freetype` can load our **in-memory pack bytes** — its API leans
  toward file paths; LVGL's `lv_ftsystem.c` should allow `FT_New_Memory_Face`, but verify. API differs
  from tiny_ttf. The font-memory Task A dedup is rasterizer-agnostic and carries over.
- **Memory:** Pi Zero uses `LV_STDLIB_CLIB` (unbounded malloc per `font-memory-plan.md`), so FreeType's
  cache is a non-issue there; the constrained-pool concern is **ESP32-only** (which keeps tiny_ttf).

## Recommendation

Revised, now that we know PIL's actual mechanism (supersample + SHARPEN, body-only):

- **Primary fix: B (supersampling, the way PIL does it — including SHARPEN).** It is a faithful,
  **script- and weight-agnostic** reproduction of what PIL ships for body text, and the
  whole-screen variant lives almost entirely in the Pi backend. **Recommended locus: whole-screen
  2× SSAA via the existing 480 profile, downscaled + sharpened in the raspi-lvgl flush.** Full design
  in the LVGL feasibility section below; concrete steps in the "Implementation plan — option B" section.
- **Complement: C (lv_freetype on Pi)** for the non-supersampled roles (titles/buttons), where PIL
  relies on hinting too. Optional, and only if those roles still read soft after B. Carries the
  open in-memory-font-load spike risk.
- **A (weight bump)** is a Latin-only tactical stand-in — only worth it for a fast visible win
  before B lands; revert if B/C make it over-bold.

## Next steps (fresh session)

1. **Spike B (whole-screen SSAA):** add `make_profile(480,480)` (shared repo); in
   `seedsigner-raspi-lvgl` build the 480 assets, register the display at 480×480, box-downscale
   480→240 + apply PIL's SHARPEN kernel in `flush_cb`; flash and photograph the **panel** vs PIL on
   the "Confirm SeedQR?" body screen. **Measure ARMv6 frame time on the screensaver/marquee** — this
   is the gating risk. (Steps detailed in the "Implementation plan — option B" section.)
2. **If animation cost is too high:** fall back to per-label body oversampling (canvas route), or
   SSAA static frames only with native-240 render during animations.
3. **C (optional complement):** confirm `lv_freetype` loads in-memory pack bytes on the Pi; add the
   compile-time rasterizer selector in `font_registry` + wire `seedsigner-raspi-lvgl`
   (`-lfreetype`, `LV_USE_FREETYPE=1`) — for titles/buttons.
4. **Independent line-spacing parity:** the tiny_ttf body line-height is looser than PIL — set
   `text_line_space` (or the body line-spacing constant) to match. Separately fixable; helps regardless
   of which rasterizer wins.
5. **Always verify on the PANEL** — zoomed screenshots hide this entirely.

## LVGL feasibility — Pi-only supersampling (verdict: feasible)

Investigated whether PIL's supersampling can be reproduced in LVGL, restricted to the Pi Zero.
**Conclusion: yes.** Concrete build steps in the "Implementation plan — option B" section below.

**What LVGL v9.5.0 gives us (vs PIL's 3 steps):**
- *Oversample render* → **yes, native.** `lv_canvas` + `lv_canvas_init_layer()` + `lv_draw_label()`
  can render text at 2×; or simply render the whole 480 profile.
- *LANCZOS downscale* → **bilinear only** (`lv_draw_sw_transform.c`, `lv_image_set_antialias(true)`)
  — but a 2×2 **box average** for the integer 2:1 case is effectively LANCZOS-equivalent.
- *SHARPEN* → **none.** Only a blur primitive exists (`lv_draw_sw_blur.c`). PIL's 3×3 unsharp must
  be **hand-rolled** (~30 lines) on the downscaled buffer.
- *tiny_ttf glyph oversampling* → **not exposed** (the bundled stb variant has no
  `PackSetOversampling`); would need a vendored-lib patch — avoided by the canvas/whole-screen route.

**Recommended locus — whole-screen 2× SSAA in the Pi flush (lives in the Pi backend):**
The existing **480 profile (`PX_MULTIPLIER_200`) is an exact geometric 2× of the 240 profile**
(`px_scale(17,200)=34=2×px_scale(17,100)`; paddings double), so:
1. Add a square `make_profile(480, 480)` in `components/seedsigner/gui_constants.cpp` — additive,
   harmless to ESP32; the only shared-repo change.
2. In `seedsigner-raspi-lvgl`: build the 480 assets (`DISPLAY_HEIGHT=480`, `setup.py`), register the
   LVGL display at **480×480**, `set_display(480,480)` so screens lay out at 2×
   (`native/python_bindings/module.cpp`, `ensure_lvgl_runtime()` ~`module.cpp:703-709`).
3. In `flush_cb` (`module.cpp:604`): **box-downscale 480→240** (RGB565 2×2 average), then apply the
   hand-rolled **SHARPEN**, then push the 240 buffer to ST7789 over the existing SPI path
   (`module.cpp:437-466`). The downscale *is* the supersample.

**Why this locus:** contained in the Pi backend + one additive profile; reuses existing 200-profile
geometry (no new layout math); uniform across every role/weight/script (the hard requirement); and
"render 2× → downscale → sharpen" *is* PIL's pipeline applied globally.

**Memory:** 480×480 RGB565 draw buffer = 460 KB (vs 115 KB now) — trivial against the Pi's
`LV_STDLIB_CLIB` unbounded heap (~490 MB headroom).

**Gating risk — ARMv6 animation cost:** 4× pixel volume per render on a ~1 GHz single core with no
NEON. Render-on-change static screens are fine; continuous animation (screensaver bounce, marquee
scroll) is the concern. **A spike must measure frame time** before committing. Mitigation: SSAA
static frames only, native-240 render during animations.

**Parity:** the 200→downscale path can differ from a native-240 render by ±1 px on odd-rounding
constants; re-validate pixel-parity **on the panel** (zoomed screenshots hide this).

**Fallback locus** — per-label body oversampling (canvas route): faithful to PIL (body-only) and
cheaper per frame, but invasive in shared `components.cpp` and conflicts with body
marquee/scroll/wrap + the `glyph_runs.cpp` shaped path. Use only if whole-screen animation cost is
unacceptable.

## Implementation plan — option B (whole-screen SSAA in the Pi flush)

The design and locus are in the "LVGL feasibility" section above; this is the concrete build
checklist and gates (folded in from the former `font-low-dpi-supersampling-plan.md`).

### Steps
1. **Shared repo (`seedsigner-lvgl-screens`) — one additive change.** Add `make_profile(480, 480)`
   in `components/seedsigner/gui_constants.cpp` (a square 2× of 240×240; `px_multiplier` derives to
   200) next to `profile_480x320` / `profile_800x480`. Harmless to ESP32 (gated under
   `SUPPORT_DISPLAY_HEIGHT_480`).
2. **Pi backend (`seedsigner-raspi-lvgl`) — the supersampling locus.**
   - Build the 480 assets: set `DISPLAY_HEIGHT=480` in `setup.py` so the 200-profile fonts (34 px
     body, etc.) and 200× logos compile in.
   - In `native/python_bindings/module.cpp` `ensure_lvgl_runtime()` (~`module.cpp:703-709`): register
     the LVGL display at **480×480**, allocate the 480×480 RGB565 draw buffer (≈460 KB), call
     `set_display(480, 480)` so screens lay out at 2×.
   - In `flush_cb` (`module.cpp:604`): **box-downscale 480→240** (RGB565 2×2 average ≈ LANCZOS for
     2:1), then **apply PIL's 3×3 SHARPEN** on the 240 output (~30 lines hand-rolled — LVGL has no
     sharpen primitive), then push to the ST7789 over the existing SPI path (`module.cpp:437-466`).
     The downscale *is* the supersample; the sharpen is the "strengthen."

### Key parameters to match PIL
- Supersample factor **2×** (480/240); downscale = 2×2 box average (area filter).
- Sharpen: 3×3 unsharp `[-2 -2 -2; -2 32 -2; -2 -2 -2] / 16` (PIL `ImageFilter.SHARPEN`). Make it a
  tunable/optional flag so parity can be dialed in on the panel.

### Verification (when built)
1. **First, the panel-gamma sim** (see "The tools flatter the rendering" above) so the desktop/web
   before/after is trustworthy — confirm native body reads faint, supersampled build reads solid.
2. Build the `.so` with the 480→240 flush; flash a Pi Zero.
3. Photograph the **panel** (not zoomed screenshots) side-by-side vs PIL on "Confirm SeedQR?" and a
   non-Latin locale (hi/th) — confirm body strokes match PIL's weight/crispness.
4. Measure frame time on the screensaver and a marquee-scrolling button to quantify the ARMv6 cost;
   decide whether the static-only mitigation is needed.

## Cross-refs
- `seedsigner/src/seedsigner/gui/components.py` `TextArea` — the supersampling source of truth.
- `docs/font-tiering-plan.md` — the baked→TTF switch; already flags TTF-vs-baked metric parity.
- `docs/knowledge/font-loading-binfont-vs-tiny-ttf.md`, `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.
- `docs/font-memory-plan.md`, `docs/knowledge/font-instance-dedup-chain-direction.md` — the *separate*
  dedup thread (PR #37). Not related to this issue.
- Evidence photos: `review/pizero-body-text/`.
