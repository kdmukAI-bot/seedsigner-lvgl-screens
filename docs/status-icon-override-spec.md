# `status_icon` override for `large_icon_status_screen` — spec

**Status:** ⚠️ DEFERRED — do NOT implement. The `seedsigner` side decided to keep
`RemoveMicroSDWarningView` deferred: it loads the "Not Yet Implemented" notice for now, so
no native glyph support is needed yet. The native approach is also **undecided** — this doc
proposes an enumerated `status_icon` key, whereas the run-screen-dispatch-refactor plan
(seedsigner `docs/_integration/run-screen-dispatch-refactor.md`, Q2) proposes a
`StatusType.CUSTOM` that accepts an explicit icon. Reconcile those two *before* implementing,
whenever microSD is revisited. Until then this is reference only.

**Consumer:** the `seedsigner` app's status-screen migration (PIL `WarningScreen` /
`DireWarningScreen` / `ErrorScreen` / `LargeIconStatusScreen` → native
`large_icon_status_screen`).

## Summary
Add an optional, **enumerated** `status_icon` key to the `large_icon_status_screen`
JSON contract that overrides **only the hero glyph**. Color and the pulsing
`warning_edges` still come from `status_type`. The allowlist currently contains a
single value, `"microsd"`; absent the key, the screen uses its `status_type` built-in
glyph exactly as today.

## Motivation
The seedsigner app is replacing its four PIL status screens with the native
`large_icon_status_screen`. That migration deliberately establishes a strict
**icon ↔ severity coupling**, driven entirely by `status_type`:

| `status_type` | glyph | color |
|---|---|---|
| `success` | check | green |
| `warning` | `!` | yellow |
| `dire_warning` | `!` | orange |
| `error` | `X` | red |

No call site overrides the hero glyph — the red `X` is reserved for genuine
device/system `error` screens, warning + dire share `!`, success shows the check.
This is what lets us retire the previous mess of, e.g., an `X` icon on an orange
screen titled "Error!".

There is exactly **one intentional exception**: the "remove the MicroSD card" prompt
(`RemoveMicroSDWarningView`, top-nav title "Action Required"). It is a warning-level
screen that deliberately shows the **microSD-card glyph** instead of the exclamation,
because the glyph itself communicates that the prompt is about the card. This was a
considered design decision in the PIL app and we want to preserve it.

To honor that affordance **without** re-opening arbitrary icon overrides (which would
let any call site re-break the coupling above), we add an explicit, enumerated
override rather than a free-form icon-name passthrough.

## Design
- New optional cfg key: `status_icon` (string).
- **Allowlist (currently one entry):** `"microsd"` → `SeedSignerIconConstants::MICROSD`
  (`""`, already present in `components/seedsigner/gui_constants.h:307`).
- **Absent** → use the `status_type` built-in glyph (`defaults.icon`). Unchanged
  behavior for every existing and migrated screen.
- **Overrides the glyph only.** `defaults.color` and `warning_edges` continue to be
  derived from `status_type`. So `{"status_type":"warning","status_icon":"microsd"}`
  renders a **yellow microSD glyph, no pulsing edges** — matching the original PIL
  `WarningScreen` + `MICROSD` look.
- **Unknown name → `throw std::runtime_error(...)`** (loud, dev-time failure),
  consistent with `parse_status_type`. We do not silently fall back, so a typo is
  caught in the implementer's own testing.
- **Explicitly not an arbitrary passthrough.** Buttons accept arbitrary icon names
  because a button glyph is not semantically coupled to anything; the status hero
  glyph's default *is* the severity signal, so its override stays curated. New named
  glyphs are added to the allowlist deliberately, one at a time, as real screens need
  them.

## Implementation
Single touch point in `components/seedsigner/seedsigner.cpp`, inside the
`large_icon_status_screen` builder. Today (≈ line 1145):

```cpp
lv_label_set_text(icon, defaults.icon);
```

Becomes:

```cpp
// Hero glyph: per-status_type default, with an explicit, enumerated override
// for the few screens that intentionally use a non-severity glyph (the
// "remove MicroSD" prompt). NOT an arbitrary icon passthrough — only known
// names are accepted, so a call site can't re-break the icon<->severity pairing.
const char* icon_glyph = defaults.icon;
if (cfg.contains("status_icon") && cfg["status_icon"].is_string()) {
    const std::string name = cfg["status_icon"].get<std::string>();
    if (name == "microsd") icon_glyph = SeedSignerIconConstants::MICROSD;
    else throw std::runtime_error("status_icon must be one of: \"microsd\"");
}
lv_label_set_text(icon, icon_glyph);
```

No font/asset work: the `MICROSD` glyph already exists in the SeedSigner icon font.

## JSON contract — what the seedsigner app sends
The consuming call site (`RemoveMicroSDWarningView`, stays warning-level):

```json
{
  "status_type": "warning",
  "status_icon": "microsd",
  "top_nav": { "title": "Action Required", "show_back_button": false },
  "text": "You must remove the\nMicroSD card to continue.",
  "button_list": ["Continue", "Settings"]
}
```

(The app passes `button_list` as `ButtonOption`s; `run_lvgl_screen` serializes them.)

## Testing & parity
- Add a **screenshot-generator scenario** (`tools/apps/screenshot_generator` +
  `tools/scenarios/scenarios.json`) for the microSD variant
  (`status_type:"warning"`, `status_icon:"microsd"`): confirm a yellow microSD hero
  glyph renders and the `!` does **not** appear.
- Confirm the **default path is unchanged**: no `status_icon` → built-in glyph
  (existing `large_icon_status_screen` scenarios should stay byte-stable).
- Confirm an **unknown `status_icon` throws**.
- **On-device:** after rebuilding the Pi `.so` and ESP32-P4 firmware, verify the glyph
  on the real "Action Required" screen on both platforms.

## Docs to update
- `docs/knowledge/large-icon-status-screen-parity.md` — note that `status_icon` exists
  as an enumerated glyph override (glyph-only; color/edges stay per `status_type`), and
  why it is curated rather than arbitrary.

## Scope, branching, guardrails
- This is a small, self-contained change to the `large_icon_status_screen` builder. It
  is **independent** of the open button-list native fixes (handoff items 6/7) and the
  font WIP — a different code path.
- Land it on **its own branch off `main`**, not the `feat/font-memory-dedup` working
  tree (where this spec file currently sits untracked).

## Acceptance criteria
- [ ] `status_icon:"microsd"` renders the microSD glyph in the `status_type`'s color.
- [ ] Absent `status_icon` → built-in glyph; no behavior change for existing screens.
- [ ] Unknown `status_icon` value → throws.
- [ ] `status_icon` does not affect color or `warning_edges`.
- [ ] Screenshot scenario added; pre-existing status scenarios remain byte-stable.
- [ ] `large-icon-status-screen-parity.md` updated.
