#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include "simple-keyboard.h"

static void dummy_focus(void) {}
static void dummy_key(uint32_t key, SkModifierType mods) { (void)key; (void)mods; }

static struct wl_surface *target_surface = NULL;
static focus_callback global_focus_enter = dummy_focus;
static focus_callback global_focus_leave = dummy_focus;
static key_callback global_press_cb = dummy_key;
static key_callback global_release_cb = dummy_key;

static struct wl_display *global_wl_display;
static int repeat_timer_fd = -1;

struct seat_node {
    uint32_t name;
    struct wl_seat *seat;
    struct seat_node *next;
};

struct repeat_data {
    uint32_t key;           // The physical key code that's repeating
    xkb_keysym_t keysym;    // The translated value at time of press
    SkModifierType modifiers;
    int32_t rate;           // repeats per second
    int32_t delay;          // initial delay in milliseconds
    struct keyboard_state *keyboard;  // Back-reference to containing keyboard
};

static struct seat_node *seat_list = NULL;

struct keyboard_state {
    struct xkb_state *xkb_state;
    struct xkb_context *xkb_context;
    struct xkb_keymap *keymap;
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose_state;
    struct wl_keyboard *keyboard;
    struct wl_seat *seat;
    struct repeat_data repeat;
    struct seat_node *seat_node;  // Back-reference to containing seat
    int has_focus;
    void *user_data;

    // Cached modifier indices for fast lookup
    xkb_mod_index_t mod_shift;
    xkb_mod_index_t mod_ctrl;
    xkb_mod_index_t mod_alt;
    xkb_mod_index_t mod_super;
    xkb_mod_index_t mod_caps;
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
static struct keyboard_state *repeating_keyboard = NULL;

static void arm_timer(int delay_ms) {
    if (repeat_timer_fd < 0) return;

    struct itimerspec ts = {0};
    ts.it_value.tv_sec = delay_ms / 1000;
    ts.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;
    // One-shot timer (it_interval stays zero)

    timerfd_settime(repeat_timer_fd, 0, &ts, NULL);
}

static void disarm_timer(void) {
    if (repeat_timer_fd < 0) return;

    struct itimerspec ts = {0};
    timerfd_settime(repeat_timer_fd, 0, &ts, NULL);
}

static SkModifierType get_modifiers_state(struct keyboard_state *kstate) {
    SkModifierType mods = 0;
    xkb_mod_mask_t mask = xkb_state_serialize_mods(kstate->xkb_state, XKB_STATE_MODS_EFFECTIVE);

    if (kstate->mod_shift != XKB_MOD_INVALID && (mask & (1 << kstate->mod_shift)))
        mods |= SK_SHIFT_MASK;
    if (kstate->mod_ctrl != XKB_MOD_INVALID && (mask & (1 << kstate->mod_ctrl)))
        mods |= SK_CONTROL_MASK;
    if (kstate->mod_alt != XKB_MOD_INVALID && (mask & (1 << kstate->mod_alt)))
        mods |= SK_ALT_MASK;
    if (kstate->mod_super != XKB_MOD_INVALID && (mask & (1 << kstate->mod_super)))
        mods |= SK_SUPER_MASK;
    if (kstate->mod_caps != XKB_MOD_INVALID && (mask & (1 << kstate->mod_caps)))
        mods |= SK_LOCK_MASK;

    return mods;
}

int keyboard_get_repeat_fd(void) {
    return repeat_timer_fd;
}

void keyboard_handle_repeat(void) {
    if (repeat_timer_fd < 0 || !repeating_keyboard)
        return;

    // Read and discard the timer expiration count
    uint64_t expirations;
    if (read(repeat_timer_fd, &expirations, sizeof(expirations)) != sizeof(expirations))
        return;

    struct keyboard_state *state = repeating_keyboard;

    // Call the press callback directly
    global_press_cb(state->repeat.keysym, state->repeat.modifiers);

    // Re-arm timer for next repeat
    if (state->repeat.rate > 0) {
        int timeout = 1000 / state->repeat.rate;
        arm_timer(timeout);
    }
}

static void stop_repeat(struct keyboard_state *kstate) {
    if (!kstate)
        return;

    if (repeating_keyboard == kstate) {
        disarm_timer();
        repeating_keyboard = NULL;
    }

    kstate->repeat.keysym = 0;
    kstate->repeat.modifiers = 0;
    kstate->repeat.keyboard = NULL;
}

static int any_keyboard_has_focus(void) {
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
    (void)keyboard;
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

    state->compose_table = xkb_compose_table_new_from_locale(
        state->xkb_context,
        setlocale(LC_CTYPE, NULL) ?: "C",
        XKB_COMPOSE_COMPILE_NO_FLAGS);

    if (state->compose_table) {
        state->compose_state = xkb_compose_state_new(
            state->compose_table,
            XKB_COMPOSE_STATE_NO_FLAGS);
    }

    // Cache modifier indices for fast lookup in hot path
    state->mod_shift = xkb_keymap_mod_get_index(state->keymap, XKB_MOD_NAME_SHIFT);
    state->mod_ctrl = xkb_keymap_mod_get_index(state->keymap, XKB_MOD_NAME_CTRL);
    state->mod_alt = xkb_keymap_mod_get_index(state->keymap, XKB_MOD_NAME_ALT);
    state->mod_super = xkb_keymap_mod_get_index(state->keymap, XKB_MOD_NAME_LOGO);
    state->mod_caps = xkb_keymap_mod_get_index(state->keymap, XKB_MOD_NAME_CAPS);

    munmap(map_str, size);
    close(fd);
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    struct keyboard_state *kstate = data;

    if (!kstate->xkb_state || !kstate->has_focus) return;

    xkb_keysym_t keysym = xkb_state_key_get_one_sym(kstate->xkb_state, key + 8);
    SkModifierType modifiers = get_modifiers_state(kstate);

    if (state == 1) { // Key press
        if (kstate->compose_state) {
            enum xkb_compose_feed_result feed =
                xkb_compose_state_feed(kstate->compose_state, keysym);

            if (feed == XKB_COMPOSE_FEED_ACCEPTED) {
                enum xkb_compose_status status =
                    xkb_compose_state_get_status(kstate->compose_state);

                switch (status) {
                case XKB_COMPOSE_COMPOSING:
                    return;  // swallow intermediate keys
                case XKB_COMPOSE_COMPOSED:
                    keysym = xkb_compose_state_get_one_sym(kstate->compose_state);
                    xkb_compose_state_reset(kstate->compose_state);
                    break;
                case XKB_COMPOSE_CANCELLED:
                    xkb_compose_state_reset(kstate->compose_state);
                    return;  // swallow the cancelled key
                case XKB_COMPOSE_NOTHING:
                    break;
                }
            }
        }

        // Only stop repeat if it's a different key
        if (kstate->repeat.keysym != keysym) {
            stop_repeat(kstate);
        }

        global_press_cb(keysym, modifiers);

        // Set up repeat if not already repeating this key
        if (kstate->repeat.rate > 0 && kstate->repeat.keysym != keysym &&
            xkb_keymap_key_repeats(kstate->keymap, key + 8)) {
            kstate->repeat.keysym = keysym;
            kstate->repeat.key = key;
            kstate->repeat.modifiers = modifiers;
            kstate->repeat.keyboard = kstate;

            repeating_keyboard = kstate;
            arm_timer(kstate->repeat.delay);
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
    (void)keyboard;
    (void)serial;
    (void)keys;
    struct keyboard_state *state = data;
    if (surface != target_surface) return;

    if (!any_keyboard_has_focus()) {
        global_focus_enter();
    }
    state->has_focus = 1;
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
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
    (void)keyboard;
    (void)serial;
    struct keyboard_state *kstate = data;
    if (kstate->xkb_state) {
        xkb_state_update_mask(kstate->xkb_state,
                              mods_depressed, mods_latched, mods_locked,
                              0, 0, group);
    }
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                        int32_t rate, int32_t delay) {
    (void)keyboard;
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
                if (current->state->compose_state) {
                    xkb_compose_state_unref(current->state->compose_state);
                    current->state->compose_state = NULL;
                }
                if (current->state->compose_table) {
                    xkb_compose_table_unref(current->state->compose_table);
                    current->state->compose_table = NULL;
                }
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

static void remove_keyboard_if_exists(struct wl_seat *seat) {
    struct keyboard_list *list = keyboard_lists;

    while (list) {
        struct keyboard_node *current = list->keyboards;

        while (current) {
            struct keyboard_node *next = current->next;  // Save next before potential removal
            if (current->state && current->state->seat == seat) {
                handle_keyboard_remove(current->state->keyboard);
                // handle_keyboard_remove already removes it from the list
                // so we don't need to do anything else here
            }
            current = next;
        }
        list = list->next;
    }
}

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     uint32_t capabilities) {
    struct seat_node *seat_node = data;

    remove_keyboard_if_exists(seat);

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
        handle_keyboard_add(keyboard, seat, seat_node);
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat,
                             const char *name) {
    (void)data;
    (void)seat;
    (void)name;
    // Optional: could store seat names if needed
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
    (void)data;
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
    (void)data;
    (void)registry;
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

void keyboard_teardown(void) {
    disarm_timer();
    repeating_keyboard = NULL;

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

    if (repeat_timer_fd >= 0) {
        close(repeat_timer_fd);
        repeat_timer_fd = -1;
    }
}

void keyboard_initialize(struct wl_surface *surface,
                         key_callback press_cb,
                         key_callback release_cb,
                         focus_callback cb_focus_enter,
                         focus_callback cb_focus_leave) {
    target_surface = surface;
    global_press_cb = press_cb;
    global_release_cb = release_cb;
    global_focus_enter = cb_focus_enter;
    global_focus_leave = cb_focus_leave;

    global_wl_display = wl_proxy_get_display((struct wl_proxy *)surface);
    if (!global_wl_display) {
        fprintf(stderr, "simple-keyboard: failed to get wl_display from surface\n");
        return;
    }

    repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (repeat_timer_fd < 0) {
        fprintf(stderr, "simple-keyboard: failed to create timerfd\n");
        return;
    }

    init_keyboard();
}
