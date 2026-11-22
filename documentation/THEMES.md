# MoonSR themes

MoonSR ships three themes: **Windows 11 Light**, **Windows 11 Dark** (the default) and **Retro** (a console look).
Choose one under **Settings > Themes**; the change applies immediately and is stored in `settings.ini` as
`[App] Theme = light | dark | retro | <imported theme key>`. If the selected imported theme file is missing or invalid at
start-up, MoonSR falls back to Windows 11 Dark.

You can add your own themes as `.moontheme` files: **Settings > Themes > Import theme...**. **Export the current theme**
writes the current theme as a template to start from. A complete sample is in `documentation/themes/example.moontheme`.

## File format

* UTF-8 text (a BOM is allowed), at most 64 KB, at most 400 lines of at most 300 characters.
* One `key = value` per line. Blank lines and lines starting with `#` or `;` are ignored. An optional `[theme]` line is allowed;
  any other `[section]` is an error.
* Keys are case-insensitive. An unknown key rejects the file. A repeated key keeps the last value.
* A key you leave out inherits from the base theme.
* Colours are `#RRGGBB` (the `#` is optional). Numbers outside their range are clamped. Booleans: `true`, `false`, `yes`, `no`,
  `on`, `off`, `1`, `0`.

## Keys

| Key | Value | Meaning |
|---|---|---|
| `name` | 1 to 40 printable characters | Required. Shown on the theme card. Two imported themes cannot share a name; a theme named like an included one is shown as "Name (custom)". |
| `base` | `light` or `dark` | Theme that supplies everything not given here. Default `dark`. |
| `background` | colour | Window background (solid; the Mica backdrop is not used when this is set). |
| `surface` | colour | Cards, tiles, menus and dialogs. |
| `border` | colour | Card, control and menu outlines. |
| `text` | colour | Main text. Control hover and pressed washes are derived from it. |
| `text_secondary` | colour | Descriptions and captions. If only `text` is given it is derived from `text` and `background`. |
| `accent` | colour | Switches, sliders, selection, links, the primary button. Hover and pressed shades are derived. |
| `accent_text` | colour | Text on `accent`. If missing, black or white is chosen for contrast. |
| `success`, `warning`, `error`, `info` | colour | Message boxes and badges. |
| `font` | font family name | Letters, digits, spaces, `- _ . ( )`, at most 64 characters. Used only if installed; otherwise the base font is used. Monospace fonts work best with the console options. |
| `radius` | 0 to 16 | Maximum corner radius in DIPs. `0` gives square corners. |
| `border_width` | 1 to 3 | Width of the thin outlines in DIPs. |
| `bracketed_controls` | boolean | Console-style controls: `[ON ]` / `[OFF]` toggles, `[ Start ]` buttons, block sliders, a `v` combo box, a block caret in number boxes, inverted (accent background) selection, boxed `INFO:` / `WARN:` messages, no icon glyphs, a console-style Home banner. |
| `prompt_headers` | boolean | `[ SECTION ]` headers, a `> Page title_` heading and a `C:\MoonSR>` status line. |

## Import rules

* The file is validated completely before anything is stored. An invalid file shows an error message and imports nothing.
* A normalised copy (not the original bytes) is stored in `%LOCALAPPDATA%\MoonSR\themes\<name>.moontheme`.
* Importing a theme whose name already exists asks whether to **replace** it or **keep both** (the new one becomes "Name (2)").
* Imported themes can be removed from their card (after a confirmation). The included themes cannot be removed.

## Safety

Theme files are treated as untrusted input. They contain no code and cannot reference other files; the only outside name they can
carry is a font family, which is used only when that family is installed.
