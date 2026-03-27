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

    [CCode (cname = "SimpleKeyboard", free_function = "keyboard_teardown", has_type_id = false)]
    [Compact]
    public class Keyboard {
    }

    [CCode (cname = "key_callback", has_target = true)]
    public delegate void KeyCallback(uint keyval, ModifierType modifiers);

    [CCode (cname = "focus_callback", has_target = true)]
    public delegate void FocusCallback();

    [CCode (cname = "compose_callback", has_target = true)]
    public delegate void ComposeCallback(string? preedit);

    [CCode (cname = "keyboard_initialize")]
    public Keyboard? initialize(
        Wl.Surface? surface,
        [CCode (delegate_target_pos = 6.1)] KeyCallback press_cb,
        [CCode (delegate_target_pos = 6.1)] KeyCallback release_cb,
        [CCode (delegate_target_pos = 6.1)] FocusCallback focus_enter_cb,
        [CCode (delegate_target_pos = 6.1)] FocusCallback focus_leave_cb,
        [CCode (delegate_target_pos = 6.1)] ComposeCallback? compose_cb
    );

    [CCode (cname = "keyboard_get_repeat_fd")]
    public int get_repeat_fd();

    [CCode (cname = "keyboard_handle_repeat")]
    public void handle_repeat();
}
