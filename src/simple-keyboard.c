#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include "simple-keyboard.h"

static void dummy_focus() {}
static void dummy_key(xkb_keysym_t key, GdkModifierType mods) {}

static struct wl_surface *target_surface = NULL;
static focus_callback global_focus_enter = dummy_focus;
static focus_callback global_focus_leave = dummy_focus;
static key_callback global_press_cb = dummy_key;
static key_callback global_release_cb = dummy_key;

static struct wl_display *global_wl_display;

static gboolean key_repeat_callback(gpointer user_data);

struct seat_node {
    uint32_t name;
    struct wl_seat *seat;
    struct seat_node *next;
};

struct repeat_data {
    uint32_t key;           // The physical key code that's repeating
    xkb_keysym_t keysym;    // The translated value at time of press
    GdkModifierType modifiers;
    guint timer;
    int32_t rate;    // repeats per second
    int32_t delay;   // initial delay in milliseconds
    struct wl_callback *callback;
    struct keyboard_state *keyboard;  // Back-reference to containing keyboard
};

static struct seat_node *seat_list = NULL;

struct keyboard_state {
    struct xkb_state *xkb_state;
    struct xkb_context *xkb_context;
    struct xkb_keymap *keymap;
    struct wl_keyboard *keyboard;
    struct wl_seat *seat;
    struct repeat_data repeat;
    struct seat_node *seat_node;  // Back-reference to containing seat
    int has_focus;
    void *user_data;
};

struct keyboard_node {
    struct keyboard_state *state;
    struct keyboard_node *next;
};

struct keyboard_list {
    struct keyboard_node *keyboards;
    struct keyboard_list *next;
};

static struct keyboard_list *keyboard_lists = NULL;

static GdkModifierType get_modifiers_state(struct xkb_state *state) {
    GdkModifierType mods = 0;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GDK_SHIFT_MASK;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GDK_CONTROL_MASK;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GDK_ALT_MASK;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GDK_SUPER_MASK;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GDK_LOCK_MASK;
    return mods;
}

static void sync_callback(void *data, struct wl_callback *callback, uint32_t time) {
    struct keyboard_state *state = data;

    wl_callback_destroy(callback);
    state->repeat.callback = NULL;

    global_press_cb(state->repeat.keysym, state->repeat.modifiers);

    if (state->repeat.rate > 0) {
        int timeout = 1000 / state->repeat.rate;
        state->repeat.timer = g_timeout_add(timeout, key_repeat_callback, state);
    }
}

static const struct wl_callback_listener sync_callback_listener = {
  sync_callback
};

static gboolean key_repeat_callback(gpointer user_data) {
    struct keyboard_state *state = user_data;
    state->repeat.callback = wl_display_sync(global_wl_display);

    wl_callback_add_listener(state->repeat.callback, &sync_callback_listener, state);

    state->repeat.timer = 0;
    return G_SOURCE_REMOVE;
}

static void stop_repeat(struct keyboard_state *kstate) {
    if (!kstate)
        return;

    if (kstate->repeat.timer) {
        g_source_remove(kstate->repeat.timer);
        kstate->repeat.timer = 0;
    }

    if (kstate->repeat.callback) {
        wl_callback_destroy(kstate->repeat.callback);
        kstate->repeat.callback = NULL;
    }

    kstate->repeat.keysym = 0;
    kstate->repeat.modifiers = 0;
    kstate->repeat.keyboard = NULL;
}

static int any_keyboard_has_focus() {
    struct keyboard_list *list = keyboard_lists;

    while (list) {
        struct keyboard_node *current = list->keyboards;
        while (current) {
            if (current->state->has_focus) {
                return 1;
            }
            current = current->next;
        }
        list = list->next;
    }
    return 0;
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                uint32_t format, int fd, uint32_t size) {
    struct keyboard_state *state = data;
    char *map_str;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    state->keymap = xkb_keymap_new_from_string(state->xkb_context, map_str,
                                             XKB_KEYMAP_FORMAT_TEXT_V1,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS);
    state->xkb_state = xkb_state_new(state->keymap);

    munmap(map_str, size);
    close(fd);
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                              uint32_t serial, uint32_t time, uint32_t key,
                              uint32_t state) {
    struct keyboard_state *kstate = data;

    if (!kstate->xkb_state || !kstate->has_focus) return;

    xkb_keysym_t keysym = xkb_state_key_get_one_sym(kstate->xkb_state, key + 8);
    GdkModifierType modifiers = get_modifiers_state(kstate->xkb_state);

    if (state == 1) { // Key press
        // Only stop repeat if it's a different key
        if (kstate->repeat.keysym != keysym) {
            stop_repeat(kstate);
        }

        global_press_cb(keysym, modifiers);

        // Set up repeat if not already repeating this key
        if (kstate->repeat.rate > 0 && kstate->repeat.keysym != keysym && xkb_keymap_key_repeats(kstate->keymap, key + 8)) {
            kstate->repeat.keysym = keysym;
            kstate->repeat.key = key;
            kstate->repeat.modifiers = modifiers;
            kstate->repeat.keyboard = kstate;

            kstate->repeat.timer = g_timeout_add(kstate->repeat.delay, key_repeat_callback, kstate);
        }
    } else if (state == 0) { // Key release
        if (kstate->repeat.key == key || (kstate->repeat.key != 0 && modifiers)) {
            stop_repeat(kstate);
        }

        global_release_cb(keysym, modifiers);
    }
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, struct wl_surface *surface,
                                struct wl_array *keys) {
    struct keyboard_state *state = data;
    if (surface != target_surface) return;

    if (!any_keyboard_has_focus()) {
        global_focus_enter();
    }
    state->has_focus = 1;
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, struct wl_surface *surface) {
    struct keyboard_state *kstate = data;

    if (surface != target_surface) return;
    kstate->has_focus = 0;

    stop_repeat(kstate);

    // Only trigger callback if no keyboard has focus anymore
    if (!any_keyboard_has_focus()) {
        global_focus_leave();
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                   uint32_t serial, uint32_t mods_depressed,
                                   uint32_t mods_latched, uint32_t mods_locked,
                                   uint32_t group) {
    struct keyboard_state *kstate = data;
    if (kstate->xkb_state) {
        xkb_state_update_mask(kstate->xkb_state,
                            mods_depressed, mods_latched, mods_locked,
                            0, 0, group);
    }
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                     int32_t rate, int32_t delay) {
    struct keyboard_state *state = data;
    state->repeat.rate = rate;
    state->repeat.delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void handle_keyboard_remove(struct wl_keyboard *keyboard) {
    if (!keyboard)
        return;

    // First release the keyboard to stop events
    wl_keyboard_release(keyboard);

    // Do a roundtrip to ensure events are processed
    wl_display_roundtrip(global_wl_display);

    // Now we can safely cleanup data structures
    struct keyboard_list *list = keyboard_lists;
    while (list) {
        struct keyboard_node *current = list->keyboards;
        struct keyboard_node *prev = NULL;

        while (current) {
            if (current->state && current->state->keyboard == keyboard) {
                // Stop any repeats
                stop_repeat(current->state);

                if (prev) {
                    prev->next = current->next;
                } else {
                    list->keyboards = current->next;
                }

                // Now safely cleanup xkb state
                if (current->state->xkb_state) {
                    xkb_state_unref(current->state->xkb_state);
                    current->state->xkb_state = NULL;
                }
                if (current->state->keymap) {
                    xkb_keymap_unref(current->state->keymap);
                    current->state->keymap = NULL;
                }
                if (current->state->xkb_context) {
                    xkb_context_unref(current->state->xkb_context);
                    current->state->xkb_context = NULL;
                }

                current->state->keyboard = NULL;
                return;
            }
            prev = current;
            current = current->next;
        }
        list = list->next;
    }
}

static void handle_keyboard_add(struct wl_keyboard *keyboard, struct wl_seat *seat, struct seat_node *seat_node) {
    struct keyboard_state *state = calloc(1, sizeof(struct keyboard_state));
    struct keyboard_node *node = calloc(1, sizeof(struct keyboard_node));
    struct keyboard_list *list = keyboard_lists;

    state->keyboard = keyboard;
    state->seat = seat;
    state->seat_node = seat_node;
    state->has_focus = 0;
    node->state = state;

    // Find or create keyboard list for this seat
    while (list) {
        if (list->keyboards && list->keyboards->state->seat == seat)
            break;
        list = list->next;
    }

    if (!list) {
        list = calloc(1, sizeof(struct keyboard_list));
        list->next = keyboard_lists;
        keyboard_lists = list;
    }

    node->next = list->keyboards;
    list->keyboards = node;

    wl_keyboard_add_listener(keyboard, &keyboard_listener, state);
}

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                  uint32_t capabilities) {
    struct seat_node *seat_node = data;

    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        struct keyboard_list *list = keyboard_lists;
        while (list) {
            struct keyboard_node *current = list->keyboards;
            while (current) {
                struct keyboard_node *next = current->next;
                if (current->state->seat == seat) {
                    handle_keyboard_remove(current->state->keyboard);
                }
                current = next;
            }
            list = list->next;
        }
    }
    else if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
        handle_keyboard_add(keyboard, seat, seat_node);
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat,
                          const char *name) {
    // Optional: could store seat names if needed
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                uint32_t name, const char *interface,
                                uint32_t version) {
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct wl_seat *seat = wl_registry_bind(registry, name,
                                             &wl_seat_interface,
                                             version < 7 ? version : 7);

        // Add to seat list
        struct seat_node *node = malloc(sizeof(struct seat_node));
        node->name = name;
        node->seat = seat;
        node->next = seat_list;
        seat_list = node;

        // Add seat listener
        wl_seat_add_listener(seat, &seat_listener, node);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                       uint32_t name) {
    struct seat_node *current = seat_list;
    struct seat_node *prev = NULL;

    while (current) {
        if (current->name == name) {
            // Find and remove all keyboards associated with this seat
            struct keyboard_list *list = keyboard_lists;
            while (list) {
                struct keyboard_node *kbd = list->keyboards;
                while (kbd) {
                    if (kbd->state->seat == current->seat) {
                        handle_keyboard_remove(kbd->state->keyboard);
                    }
                    kbd = kbd->next;
                }
                list = list->next;
            }

            if (prev) {
                prev->next = current->next;
            } else {
                seat_list = current->next;
            }

            wl_seat_destroy(current->seat);
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void init_keyboard(void) {
    struct wl_registry *registry = wl_display_get_registry(global_wl_display);

    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(global_wl_display); // Ensure we get all initial seats
}

void teardown() {
    while (keyboard_lists) {
        struct keyboard_list *current_list = keyboard_lists;
        keyboard_lists = current_list->next;

        while (current_list->keyboards) {
            struct keyboard_node *current = current_list->keyboards;
            current_list->keyboards = current->next;

            if (current->state) {
                handle_keyboard_remove(current->state->keyboard);
                free(current->state);
            }
            free(current);
        }
        free(current_list);
    }
    wl_display_roundtrip(global_wl_display);

    struct seat_node *seat = seat_list;
    while (seat) {
        struct seat_node *next = seat->next;

        // Then destroy the seat
        wl_seat_destroy(seat->seat);
        free(seat);
        seat = next;
    }
    wl_display_roundtrip(global_wl_display);

    seat_list = NULL;
    keyboard_lists = NULL;
}

void initialize(struct wl_surface *surface,
               key_callback press_cb,
               key_callback release_cb,
               focus_callback cb_focus_enter,
               focus_callback cb_focus_leave) {
    target_surface = surface;
    global_press_cb = press_cb;
    global_release_cb = release_cb;
    global_focus_enter = cb_focus_enter;
    global_focus_leave = cb_focus_leave;

    GdkWaylandDisplay *wl_display = GDK_WAYLAND_DISPLAY(gdk_display_get_default());
    if (!wl_display) {
        printf("Not running under Wayland\n");
        return;
    }

    global_wl_display = gdk_wayland_display_get_wl_display(wl_display);
    init_keyboard();
}
