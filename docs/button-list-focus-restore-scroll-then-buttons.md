# button_list_screen — focus restore is lost on re-render (scroll-then-buttons)

_Status: **native fix IMPLEMENTED (2026-06-26); seedsigner-side forwarding is the remaining
end-to-end piece.** Surfaced on-device (Pi Zero) during the `seedsigner`-side settings-multiselect
migration (2026-06-25). Companion: `components/seedsigner/navigation.cpp`,
`components/seedsigner/seedsigner.cpp` (`bind_screen_navigation`)._

> **Fix as built (native) — unified rule via the default-index value.** A button is active by
> default; the only time nothing is highlighted is when scrolling is genuinely required to reach the
> buttons. This is carried by `nav_config_t.initial_body_index`:
> - **Concrete index → stays focused even when the body overflows** (scrolled into view). Button
>   lists / menus pass **0**; an explicit `initial_selected_index` (a focus-restore on re-render)
>   flows through the same path. So a button list always has a selection — including on return to a
>   multi-select.
> - **`NAV_INDEX_NONE` → "no forced default"**: the first item is focused when the screen FITS, but
>   the screen starts UNFOCUSED (read-first) while scrolling is required. Status / warning screens
>   pass this, so a long warning makes the user scroll before the OK button is selectable, while a
>   short one shows it active immediately.
>
> `nav_bind`'s overflow branch keeps focus iff `initial_body_index != NAV_INDEX_NONE`. (This replaced
> the earlier separate `initial_index_explicit` flag — the index value alone now expresses intent.)
> Verified headlessly: `intro_text` (overflow, nothing → **"QR code" / index 0 active**);
> `intro_text_restore` (explicit 4 → **"Sparrow"**); `checkbox_multi` (overflow, nothing → **"Native
> SegWit" / index 0 active + checked**); `checkbox_multi_restore` (explicit 5 → **"Coordinator
> default"** restored); `large_icon_status warning` (fits → **"Continue" active**);
> `large_icon_status long_text_overflow` (overflow → **read-first, no highlight**).

> **Multi-select coverage — "returning to a multi-select should show a selected button."** Now holds
> two ways: (1) when the seedsigner side forwards the toggled row as `initial_selected_index`, the
> native restore highlights it; AND (2) even when **nothing** is forwarded, the new default selects
> **index 0** — so the screen is never left with no selection. The native side is `button_style`-
> agnostic (verified for checkbox). **Remaining (seedsigner repo):** forward `selected_button` →
> `initial_selected_index` on the multiselect re-render so the *correct* row (not just 0) is restored;
> the `button_list_lvgl_cfg` / `to_lvgl()` forwarding leaf. **The index-0 special-case is now moot:**
> the old `if selected_button:` guard that drops 0 is harmless — a dropped 0 falls back to the index-0
> default, which is the right row anyway; no `restore_selected_index` distinct-signal key is needed.

## Symptom

On a `button_list_screen` used as a **settings multiselect** (checkbox list), after the user toggles an
option the screen re-renders and **no button is highlighted/active** — the list has jumped to the top and
the user must press a joystick direction (DOWN) to re-engage a button. Single-select (radio /
`checked_selection`) settings that are short don't show it; long ones would show a related first-render
variant (current value not highlighted on entry).

## Root cause

`bind_screen_navigation` auto-enables **scroll-then-buttons** mode for a vertical screen with non-focusable
intro content (`upper_body != body`) whose body overflows the viewport — explicitly including *"a
button_list_screen with intro text"* (`seedsigner.cpp`, detection ~lines 267–275; comment ~242–249). In that
mode `nav_bind` does (`navigation.cpp` ~455–458):

```cpp
if (ctx->scroll_then_buttons && ctx->scroll_obj) {
    ctx->zone = NAV_ZONE_SCROLL;                          // overrides the body-button focus
    lv_obj_scroll_to_y(ctx->scroll_obj, 0, LV_ANIM_OFF);  // jump to top, nothing highlighted
}
```

This **overrides** the `init` body index that was just computed from `initial_selected_index`
(`navigation.cpp` ~443–448). The intent is correct for read-heavy screens (warnings/notices/long intro): the
user reads from the top, first DOWN scrolls. But it unconditionally discards an **explicit** focus-restore
request from the caller.

## Why it surfaced now

The `seedsigner` settings selection screen (`SettingsEntryUpdateSelectionView`) was migrated from PIL to
`button_list_screen`. It passes the setting name + help text as intro `text`, and for **multiselect** it
**re-renders the whole screen in place on every toggle**, forwarding `initial_selected_index = <the toggled
row>` to restore focus there. Each rebuild re-triggers the scroll-then-buttons reset, so the restore is
dropped every toggle. The PIL screen never had this — it re-highlighted `selected_button` on every render.
This native code path (scroll-then-buttons + an explicit restore index + repeated re-render) was simply never
exercised before the settings migration.

## Native fix (as built — supersedes the original proposal below)

The as-built fix is the unified "default-index value carries intent" rule in the **Fix as built**
note at the top of this doc: button lists pass a concrete default index (`0`) that stays focused even
under overflow; status/warning screens pass `NAV_INDEX_NONE` (read-first only while scrolling is
required); an explicit `initial_selected_index` flows through the same concrete-index path. This is
simpler than — and replaces — the original "separate explicit bit" proposal:

> _Original proposal (superseded):_ thread a "was explicit" bit
> (`cfg.contains("initial_selected_index")`) into `nav_bind` and skip the no-highlight override only
> when a restore index was given. This worked but needed an extra `nav_config_t` flag; the
> default-index-value approach expresses the same intent without it.

Secondary benefit (delivered): the first-render case of **long single-select** lists also highlights
its current value on entry (concrete index) rather than starting blank.

## Coordinated seedsigner-side follow-up

The native side restores whatever index it's given; the remaining end-to-end work is on the seedsigner
side: **forward `selected_button` → `initial_selected_index`** on the multiselect re-render (the
`button_list_lvgl_cfg` / `to_lvgl()` forwarding leaf) so the *correct* toggled row is restored, not
just the index-0 default.

`button_list_lvgl_cfg` currently guards `if selected_button:` before forwarding, which drops index 0.
**That is now harmless and needs no special contract:** a dropped 0 → nothing forwarded → the native
**index-0 default** lands on the right row anyway. So forwarding any *non-zero* `selected_button` (the
existing truthiness guard already does this) is sufficient; no `restore_selected_index` key is needed.
Forwarding 0 unconditionally is still unnecessary (the default covers it) and would be a no-op.

## References

- `components/seedsigner/navigation.cpp` — `nav_bind` (~396), initial index (~443–448), scroll-then-buttons
  override (~455–458).
- `components/seedsigner/seedsigner.cpp` — `bind_screen_navigation` detection (~267–275, comment ~242–249);
  `nav_initial_index_from_cfg` `cfg.contains` discriminator (~228–236).
- `seedsigner` repo: `SettingsEntryUpdateSelectionView` (`views/settings_views.py`) — re-renders in place on
  every multiselect toggle, forwarding `selected_button = ret_value`; `button_list_lvgl_cfg`
  (`views/view.py`) — the `if selected_button:` truthiness guard.
