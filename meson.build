project('simple-keyboard', 'c')
project_name = 'io.github.trbjo.vala-libraries.simple-keyboard'

gtk_dep = dependency('gtk4')
wayland_dep = dependency('wayland-client')
xkbcommon_dep = dependency('xkbcommon')

inc = include_directories('src')

vapi_dir = meson.current_source_dir()

simple_keyboard_lib = static_library('simple-keyboard',
    sources: [ 'src/simple-keyboard.c' ],
    include_directories: inc,
    dependencies: [gtk_dep, wayland_dep, xkbcommon_dep],
    install: false,
    c_args: ['-D_GNU_SOURCE'],
)

simple_keyboard_dep = declare_dependency(
    link_with: simple_keyboard_lib,
    include_directories: [inc, include_directories('.')],
    dependencies: [gtk_dep, wayland_dep],
    variables: {'vapi_dir': vapi_dir}
)
