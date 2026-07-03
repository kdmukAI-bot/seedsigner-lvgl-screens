# Static screenshots freeze the pre-animation frame (no `lv_tick_inc`)

## Symptom
A screen that should appear *repositioned by an animation* renders in its **pre-animation**
state in the static PNG gallery, even though it behaves correctly on device and in the
interactive runner.

Concrete case that surfaced this: rendering `button_list_screen` with a high
`initial_selected_index` (e.g. `9` on a 10-item list). The expectation is that the list
scrolls so the selected item is visible (this is how settings screens restore position on
back-navigation — see `docs/button_list_screen_parity.md`). The static PNG instead shows the
list at the **top**, item 0 visible, selected item off-screen — looking like the scroll
restore is broken. It isn't.

## Root cause
LVGL animations only advance when the LVGL clock advances (`lv_tick_inc`) *and* the timer
handler runs. The screenshot generator's **static PNG path** does neither for animations —
it ticks zero milliseconds:

```c
// tools/apps/screenshot_generator/screenshot_gen.cpp  (static PNG capture, ~line 686)
lv_timer_handler();   // run once
lv_refr_now(disp);    // force a redraw
// NOTE: no lv_tick_inc() — the LVGL clock never advances
```

So any animation kicked off during screen construction is created but never stepped. The
capture is frame 0.

The focus/scroll restore is animated: when the nav layer focuses the initial body item it
calls

```c
// components/seedsigner/navigation.cpp  (update_visual_focus, ~line 194)
lv_obj_scroll_to_view(ctx->body_items[ctx->body_index], LV_ANIM_ON);
```

`LV_ANIM_ON` means the scroll happens over time. With the clock frozen at 0 ms, the static
PNG shows the list before the scroll has moved. The same applies to anything else animated:
marquee label scroll (its begin-hold and motion), fades, etc. — the static PNG shows their
*start* position.

## Why it's not a bug
On the paths that DO advance the clock, the animation plays and the screen looks right:

- **Animated GIF path** — `screenshot_gen.cpp` ~line 395 ticks every frame
  (`lv_tick_inc(frame_step_ms); lv_timer_handler(); lv_refr_now(disp);`), so the scroll/marquee
  animate through.
- **Interactive runner** — `tools/apps/runner_core/runner_core.cpp` ~line 171 ticks real
  elapsed time each loop (`lv_tick_inc(elapsed_ms); lv_timer_handler();`).
- **Device** — the real tick source runs continuously.

Proof it's purely a capture artifact: temporarily switching the scroll to `LV_ANIM_OFF`
(immediate) makes the static PNG show the list correctly scrolled to the selected item. (Don't
commit that — `LV_ANIM_ON` is the right on-device behavior; it was only a diagnostic.)

## How to actually see the post-animation state
- Render the scenario through the **GIF path** (or bump `animation_seconds`) and look at a
  later frame, or
- Run it in the **interactive runner**, or
- For a one-off diagnostic only, flip the relevant `LV_ANIM_ON` → `LV_ANIM_OFF`, capture, then
  revert.

Do **not** "fix" this by adding ticks to the static PNG path: the byte-identical screenshot
regression gate depends on the static capture being deterministic and animation-free. An
animated capture would be time-dependent and defeat the gate. The static PNG is meant to show
the settled, non-animated layout; treat animated end-states as out of scope for it.

## Takeaway
When a static screenshot seems to show an animation-driven layout in the wrong position,
suspect the frozen clock before suspecting the screen logic. Animations are constructed but
not stepped in the static PNG path (no `lv_tick_inc`); verify via the GIF path, the runner, or
device.
