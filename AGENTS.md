# AGENTS.md

Repository-local rules for coding agents working on `bmenu`.

## Project Intent

- Keep `bmenu` tiny, fast to start, and keyboard-first.
- Prioritize GNOME/Mutter Wayland behavior over broad feature scope.
- Preserve dmenu-like scripting contract: stdin in, stdout selection out.
- Avoid outright implementing guards/fallbacks

## Comment And Docstring Style

- Use lean comments, not tutorial prose.
- Add docstrings for structs and functions.
- For protocol-required no-op callbacks, say that directly in one line.

## Output Character Style

Use plain ASCII. Replace fancy variants with their plain equivalents:

- Curly quotes `’ ‘ “ ” „` -> `'` or `"`.
- Long dashes `– — − ‑ ‒` -> `-`.
- Ellipsis `…` -> `...`.
- Fullwidth forms `： ｜ ⧸ ＜ ＞` -> `: | / < >`.
- Invisible whitespace (U+200B zero-width space, U+202F narrow no-break space) -> omit.

Applies to prose, Markdown, code comments, commit messages, and any other
generated text.

## UX And Behavior Defaults

- `Esc` cancels without stdout output.
- `Enter` prints selected value only.
- Arrow-key navigation must remain predictable and fast.
- Filtering should stay responsive and simple.

## Build And Tooling

- Prefer `make` targets for common workflows.
- Use Meson as the underlying build system.

## Safety

- Never revert unrelated user changes.
- Avoid destructive git commands unless explicitly asked.
- Never commit.
