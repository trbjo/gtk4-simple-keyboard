#ifndef WAYLAND_KBD_H
#define WAYLAND_KBD_H

#include <stdint.h>

typedef void (*key_callback)(uint32_t keyval, GdkModifierType modifiers);
typedef void (*focus_callback)();

void initialize(struct wl_surface *target_surface,
                key_callback press_cb,
                key_callback release_cb,
                focus_callback cb_focus_enter,
                focus_callback cb_focus_leave);

void teardown();

#endif /* WAYLAND_KBD_H */
