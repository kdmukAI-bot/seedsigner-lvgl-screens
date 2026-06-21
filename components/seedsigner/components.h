#ifndef SEEDSIGNER_COMPONENTS_H
#define SEEDSIGNER_COMPONENTS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>

lv_obj_t* top_nav(lv_obj_t* lv_parent, const char *title, bool show_back_button, bool show_power_button, lv_obj_t **out_back_btn, lv_obj_t **out_power_btn, const lv_font_t *title_font = nullptr);

// Configure an already start-justified, width-constrained single-line label to
// continuously marquee-scroll (circular wrap) an overflowing line with an initial
// hold + a hold on each wrap to the start, at a steady ~40 px/sec. Used by the
// top-nav title and long status headlines. Shaped (hi/th) labels ride the same scroll
// offset via the glyph-run draw (Task 0); RTL (ur) is excluded there for now.
void label_set_line_autoscroll(lv_obj_t* label);

typedef struct {
    const char *label;
    void *value;
} button_list_item_t;

lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to);
lv_obj_t* large_icon_button(lv_obj_t* lv_parent, const char* icon, const char* text, lv_obj_t* align_to);
lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count);

void button_set_active(lv_obj_t* lv_button, bool active);

// Hardware/joystick focus: promote a focused button's too-wide text label to a
// marquee scroll (true) or clip it back to its start edge (false). Driven from the
// nav layer because body buttons are kept out of the LVGL focus group, so LVGL
// never emits the FOCUSED/DEFOCUSED events that would otherwise drive this.
void button_set_label_marquee(lv_obj_t* lv_button, bool marquee);


#endif // SEEDSIGNER_COMPONENTS_H
