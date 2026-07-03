# Restored list selection animates into view on initial render (`button_list_screen`)

## Symptom
Re-entering a `button_list_screen` that restores a previously-selected row near the
bottom of an *overflowing* list (e.g. an Advanced Settings option) plays a visible
scroll animation from the top of the list down to that row. The legacy PIL screen
rendered the already-scrolled state directly, with no animation. It is functionally
correct (it lands on the right row); only the initial presentation differs.

## Root cause
On bind, `bind_screen_navigation()` (`navigation.cpp`, the `scroll_then_buttons` +
concrete `initial_body_index` "keep_focus" path) correctly scrolls the restored item
into view with **`LV_ANIM_OFF`** (a direct jump). It then calls
`update_visual_focus(ctx)`, which **unconditionally** re-scrolls the focused body item
with **`LV_ANIM_ON`** (the `lv_obj_scroll_to_view(..., LV_ANIM_ON)` in
`update_visual_focus`). So on the very first render the focused item gets *animated*
into view instead of appearing there directly. `LV_ANIM_ON` is the desired behavior
for *user-driven* navigation; it is only wrong on the initial render.

(Reference points at submodule `03356df`: the init jump is the `LV_ANIM_OFF`
`lv_obj_scroll_to_view` in `bind_screen_navigation` ~navigation.cpp:477; the
re-animation is the `LV_ANIM_ON` `lv_obj_scroll_to_view` in `update_visual_focus`
~navigation.cpp:193, called from ~:485.)

## Fix
Gate the focus scroll so the **initial** render uses `LV_ANIM_OFF` (direct jump,
matching PIL) and only *subsequent* navigation-driven focus changes animate
(`LV_ANIM_ON`). Either:
- pass an `animate` flag into `update_visual_focus()` (the bind-time call passes
  `false`), or
- carry a one-shot "initial render done" flag on `nav_ctx_t`.

The explicit `LV_ANIM_OFF` scroll in `bind_screen_navigation` then becomes redundant
(once `update_visual_focus` already jumps on the first render) and can be dropped.

## Context
- Surfaced during on-device testing (Pi Zero) of the seedsigner `run_screen` refactor,
  which restores the selection via `selected_button -> initial_selected_index`. The
  Python side is correct — this is purely native presentation, and pre-dates that
  refactor (any settings re-render that restores a bottom row hits it).
- Related: [`button-list-focus-under-overflow.md`](button-list-focus-under-overflow.md)
  (the focus-restore behavior this animation sits on top of).
