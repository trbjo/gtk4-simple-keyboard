[CCode (cheader_filename = "simple-keyboard.h")]
namespace SimpleKeyboard {
    [CCode (cname = "key_callback", has_target = false)]
    public delegate void KeyCallback(uint keyval, Gdk.ModifierType modifiers);

    [CCode (cname = "focus_callback", has_target = false)]
    public delegate void FocusCallback();

    [CCode (cname = "initialize")]
    public void initialize(Wl.Surface? surface, KeyCallback press_cb, KeyCallback release_cb, FocusCallback focus_enter_cb, FocusCallback focus_leave_cb);

    [CCode (cname = "teardown")]
    public void teardown();
}
