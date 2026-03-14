#ifndef SIMPLE_KEYBOARD_H
#define SIMPLE_KEYBOARD_H

#include <stdint.h>

// Modifier flags (matches GDK4 values for compatibility)
typedef enum {
    SK_SHIFT_MASK   = 1 << 0,
    SK_LOCK_MASK    = 1 << 1,  // Caps lock
    SK_CONTROL_MASK = 1 << 2,
    SK_ALT_MASK     = 1 << 3,
    SK_SUPER_MASK   = 1 << 26,
} SkModifierType;

struct wl_surface;

typedef void (*key_callback)(uint32_t keyval, SkModifierType modifiers);
typedef void (*focus_callback)(void);

// Initialize keyboard handling for the given surface.
// The wl_display is derived from the surface automatically.
void keyboard_initialize(struct wl_surface *target_surface,
                         key_callback press_cb,
                         key_callback release_cb,
                         focus_callback cb_focus_enter,
                         focus_callback cb_focus_leave);

// Returns the file descriptor for key repeat timing.
// Add this to your event loop (poll/epoll/etc) and call
// keyboard_handle_repeat() when it becomes readable.
// Returns -1 if repeat is not active.
int keyboard_get_repeat_fd(void);

// Call this when keyboard_get_repeat_fd() is readable.
void keyboard_handle_repeat(void);

// Cleanup all keyboard resources.
void keyboard_teardown(void);

#endif /* SIMPLE_KEYBOARD_H */
