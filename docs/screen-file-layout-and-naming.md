# Screen file layout & naming convention

This document describes how the C/C++ screen code in `components/seedsigner/` is
organized and — the part that carries the most weight for day-to-day work — how
screens are named and why.

## File layout

All portable screen code lives under `components/seedsigner/`.

```
components/seedsigner/
├── seedsigner.h              Public C API header: every screen entry point, the
│                             lifecycle + host-callback surface. The one header a
│                             host (Pi Zero / ESP32) includes.
├── screens/                  One translation unit per screen entry point.
│   ├── main_menu_screen.cpp
│   ├── button_list_screen.cpp
│   ├── seed_finalize_screen.cpp
│   ├── psbt_overview_screen.cpp
│   ├── opening_splash_screen.cpp
│   └── … (one *_screen.cpp per screen)
│
├── screen_scaffold.{h,cpp}   Shared screen chrome: TopNav + body construction,
│                             joystick/touch navigation binding, screen load/cleanup,
│                             the WarningEdges overlay. Every screen builds its shell
│                             through these entry points.
├── screen_helpers.{h,cpp}    Cross-cutting render/layout helpers used across domains:
│                             button-list config readers, status-type tables,
│                             text ink-measure + tight line-spacing, hex/network color,
│                             the btc_amount builder, and the static-render mode flag.
├── qr_core.{h,cpp}           QR encode/decode core + the shared gutter "X" close
│                             button, compiled once and shared by the chrome-free QR
│                             screens (qr_display, seed_transcribe_zoomed_qr).
├── components.{h,cpp}        LVGL widget builders (top_nav, buttons, button lists,
│                             btc_amount, …) and the module's weak host-callback
│                             defaults (on_button_selected / on_text_entered) +
│                             lv_seedsigner_screen_close.
├── navigation.{h,cpp}        Hardware input navigation (joystick + KEY1–3).
├── input_profile.{h,cpp}     Touch vs. hardware input mode selection.
├── gui_constants.{h,cpp}     Colors, fonts, layout constants, display profiles.
├── glyph_runs / locale_* / font_registry / keyboard_core / overlay_manager / camera_*
│                             Supporting subsystems (i18n shaping, locale packs,
│                             fonts, keyboard mechanics, screensaver dispatch, camera
│                             overlays).
└── screen_sources.cmake      The single source of truth that globs screens/*_screen.cpp.
```

### Build wiring

- **Per-screen files are globbed, not listed.** `screen_sources.cmake` does
  `file(GLOB … CONFIGURE_DEPENDS "screens/*_screen.cpp")` and is `include()`d by all
  five build lists (the ESP32 component `CMakeLists.txt` plus the four desktop tools:
  screenshot_generator, screen_runner, web_runner, runner_core test). **Adding a
  screen needs zero build-list edits** — the glob picks it up.
- **Shared infra `.cpp` files are listed explicitly** in each of the five build lists'
  infra section (they are not `*_screen.cpp`, so the glob does not catch them):
  `components.cpp`, `navigation.cpp`, `input_profile.cpp`, `qr_core.cpp`,
  `screen_scaffold.cpp`, `screen_helpers.cpp`, etc.
- There is **no `seedsigner.cpp`** — only the `seedsigner.h` public header. The former
  monolith's contents now live in `screens/` (screen bodies), `screen_scaffold.cpp` /
  `screen_helpers.cpp` (shared helpers), and `components.cpp` (host hooks + screen_close).

## Naming convention

### The invariant

For every screen, **one canonical string** is reused verbatim across four places —
zero transformation:

```
filename (screens/<name>.cpp)  ==  entry symbol (void <name>(void *ctx_json))
                               ==  registry key ({"<name>", <name>})
                               ==  scenario name (top-level key in scenarios.json)
```

So a screen named `opening_splash_screen` is `screens/opening_splash_screen.cpp`,
defines `void opening_splash_screen(void*)`, is registered as
`{"opening_splash_screen", opening_splash_screen}` in both name→function registries
(`runner_core.cpp`, `screenshot_gen.cpp`), and is keyed `"opening_splash_screen"` in
`scenarios.json`. A rename means changing all four together (plus the `seedsigner.h`
declaration) — never just the file.

Renaming is therefore a public-contract change: the two registries, the header, the
scenario JSON keys (base + all localized copies), and any host that calls the symbol by
name (the Pi Zero / ESP32 bindings) all move together.

### How the name is chosen

**Default to the Python screen class name, converted to `snake_case`.** The Python app
in `seedsigner/` is the driver; deriving the C++ name mechanically from the Python class
keeps the two codebases greppable against each other and makes paths predictable.

The domain prefixes are **not** a separate grouping scheme layered on top — they are
simply *part of the Python class names*:

| Python class (`gui/screens/…`)   | C++ screen name                    |
| -------------------------------- | ---------------------------------- |
| `SettingsQRConfirmationScreen`   | `settings_qr_confirmation_screen`  |
| `ToolsCalcFinalWordScreen`       | `tools_calc_final_word_screen`     |
| `SeedFinalizeScreen`             | `seed_finalize_screen`             |
| `MultisigWalletDescriptorScreen` | `multisig_wallet_descriptor_screen`|
| `OpeningSplashScreen`            | `opening_splash_screen`            |
| `QRDisplayScreen`                | `qr_display_screen`                |

A consequence worth calling out: `MultisigWalletDescriptorScreen` lives in the Python
module `seed_screens.py`, but the *class name* carries no `Seed`/`Psbt` prefix — so the
C++ name is `multisig_wallet_descriptor_screen`, **not** `seed_…` or `psbt_…`. The class
name is the authority, not the module file it happens to sit in.

### When to deviate from the Python name

The Python class name is a **strong guide, not an absolute rule.** Deviate when:

1. **The Python name is imprecise.** Prefer the clearer name and update the C++ side
   even if it diverges. (This is why we do *not* mirror an imprecise class name blindly.)
2. **There is no Python screen class** — a screen that only exists in the LVGL layer.
   Choose a clear, descriptive name, prefixed by the domain of the driving View when one
   applies:
   - `loading_spinner_screen` — an LVGL-only busy spinner (no Python `LoadingScreen`
     class); "spinner" names what it is and disambiguates it from the opening splash.
   - `settings_locale_picker_screen` — the endonym locale picker (no Python screen
     class; its driving `LocaleSelectionView` lives in `settings_views.py`, so it takes
     the `settings_` domain prefix).

The hard constraint in every case is the invariant above: whatever the name, it is the
one canonical string used for the file, the symbol, the registry key, and the scenario.

### Why this scheme

The corpus is AI-primary — navigated, reviewed, and edited far more by tools than by a
human scrolling one big file. Small single-purpose files with name-derivable paths and a
zero-transform contract make a screen trivially locatable from any one of its four names,
keep the build mechanical (glob), and let the C++ layer be cross-referenced against the
Python source by name.

## Adding a new screen

1. Create `components/seedsigner/screens/<name>_screen.cpp` defining
   `void <name>_screen(void *ctx_json)`. Build its chrome through `screen_scaffold.h`.
2. Declare it in `seedsigner.h`.
3. Register it in both name→function registries: `tools/apps/runner_core/runner_core.cpp`
   and `tools/apps/screenshot_generator/screenshot_gen.cpp`.
4. Add a `"<name>_screen"` scenario to `tools/scenarios/scenarios.json` (and localized
   copies if it carries user-facing text).

No CMake edit is needed — `screen_sources.cmake` globs the new file automatically.
