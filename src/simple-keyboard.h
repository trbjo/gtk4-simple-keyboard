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
typedef struct SimpleKeyboard SimpleKeyboard;

typedef void (*key_callback)(uint32_t keyval, SkModifierType modifiers, void *user_data);
typedef void (*focus_callback)(void *user_data);
typedef void (*compose_callback)(const char *preedit, void *user_data);

// Initialize keyboard handling for the given surface.
// The wl_display is derived from the surface automatically.
// Global state (registry, seats, timer) is set up lazily on first call.
// Returns a handle for this surface's keyboard instance.
SimpleKeyboard* keyboard_initialize(struct wl_surface *surface,
                                     key_callback press_cb,
                                     key_callback release_cb,
                                     focus_callback focus_enter_cb,
                                     focus_callback focus_leave_cb,
                                     compose_callback compose_cb,
                                     void *user_data);

// Returns the file descriptor for key repeat timing.
// Add this to your event loop (poll/epoll/etc) and call
// keyboard_handle_repeat() when it becomes readable.
// Returns -1 if repeat is not active.
int keyboard_get_repeat_fd(void);

// Call this when keyboard_get_repeat_fd() is readable.
void keyboard_handle_repeat(void);

// Remove this surface's keyboard instance.
// Cleans up global state when the last instance is removed.
void keyboard_teardown(SimpleKeyboard *kb);

#endif /* SIMPLE_KEYBOARD_H */
