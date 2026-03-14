[CCode (cheader_filename = "simple-keyboard.h")]
namespace SimpleKeyboard {
    [CCode (cname = "SkModifierType", cprefix = "SK_", has_type_id = false)]
    [Flags]
    public enum ModifierType {
        SHIFT_MASK   = 1 << 0,
        LOCK_MASK    = 1 << 1,
        CONTROL_MASK = 1 << 2,
        ALT_MASK     = 1 << 3,
        SUPER_MASK   = 1 << 26,
    }

    [CCode (cname = "key_callback", has_target = false)]
    public delegate void KeyCallback(uint keyval, ModifierType modifiers);

    [CCode (cname = "focus_callback", has_target = false)]
    public delegate void FocusCallback();

    [CCode (cname = "keyboard_initialize")]
    public void initialize(Wl.Surface? surface, KeyCallback press_cb, KeyCallback release_cb, FocusCallback focus_enter_cb, FocusCallback focus_leave_cb);

    [CCode (cname = "keyboard_get_repeat_fd")]
    public int get_repeat_fd();

    [CCode (cname = "keyboard_handle_repeat")]
    public void handle_repeat();

    [CCode (cname = "keyboard_teardown")]
    public void teardown();
}
