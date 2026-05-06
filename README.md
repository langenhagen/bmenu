# bmenu

![bmenu logo](res/bmenu.png)

A tiny, fast dmenu-inspired menu for GNOME/Mutter/Wayland, written in C.

`bmenu` is a `dmenu`-like picker for systems where wlroots-only tools are a bad fit.  
It reads newline-separated values from `stdin`, shows them in a small GUI list, fuzzy filters while
typing, and prints the selected value to `stdout`.

## Features
- Quick startup, simple behavior.
- Reads items from `stdin`, prints selection to `stdout`.
- Fuzzy case-insensitive filtering.
- Keyboard navigation.

## Project Structure
```text
.
├── AGENTS.md               Repository-local agent rules.
├── explore-with-bmenu.sh   Sample app; a imple file explorer using bmenu.
├── LICENSE                 License file.
├── main.c                  Main program.
├── Makefile                Minimal build/run/test shortcuts.
├── meson.build             Build definition.
├── README.md               You are here now.
└── test-bmenu.sh           Tiny manual smoke test.
```

## Dependencies
On Ubuntu:
```bash
sudo apt install meson ninja-build pkg-config \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libcairo2-dev libpango1.0-dev
```

## Build
```bash
make
```

## Usage
Pipe values into `bmenu`:
```bash
printf '%s\n' alpha beta gamma | ./build/bmenu
```

Type to filter, arrow keys to navigate, `Enter` to confirm.  
The selected value is printed to stdout; on cancel or error, `bmenu` exits non-zero with no output.

### Keybindings
- `Up` / `Down` - move selection.
- `Enter` - print selection to stdout and exit `0`.
- `Esc` - cancel without output, exit non-zero.
- `BackSpace` - delete previous character.
- `Ctrl+W` - delete previous word.
- `Ctrl+U` - clear the query.
- `PgUp`/`PgDn` jump up/down.

## Quick Test
```bash
make test
```

Feeds the Greek alphabet into `bmenu` so you can try filtering and scrolling.

## explore-with-bmenu.sh
`explore-with-bmenu.sh` is a simple file explorer that uses `bmenu` as its picker.  
It is based on [explore-with-dmenu](https://github.com/langenhagen/explore-with-dmenu) and reads
the same `edmrc` config from `${XDG_CONFIG_HOME:-$HOME/.config}/edm/edmrc`, so existing `.edmrc`
setups work unchanged.

Run from the repo root:
```bash
./explore-with-bmenu.sh
```

## License
See [LICENSE](LICENSE) file.
