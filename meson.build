project('bmenu', 'c',
    version: '0.1.0',
    default_options: [
        'warning_level=2',
        'c_std=c11',
        'buildtype=release',
        'b_lto=true',
        'strip=true',
    ]
)

deps = [
    dependency('wayland-client'),
    dependency('wayland-protocols'),
    dependency('xkbcommon'),
    dependency('cairo'),
    dependency('pangocairo'),
]

wayland_scanner = find_program('wayland-scanner')
wayland_protocols = dependency('wayland-protocols')
protocol_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
xdg_shell_xml = protocol_dir / 'stable/xdg-shell/xdg-shell.xml'

xdg_shell_client_header = custom_target('xdg-shell-client-header',
    input: xdg_shell_xml,
    output: 'xdg-shell-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

xdg_shell_private_code = custom_target('xdg-shell-private-code',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)

executable('bmenu',
    ['main.c', xdg_shell_private_code, xdg_shell_client_header],
    dependencies: deps,
    install: false
)
