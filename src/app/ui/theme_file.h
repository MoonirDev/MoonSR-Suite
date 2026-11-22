// Custom themes: the ".moontheme" text format, its parser and serialiser, the palette it produces, and the catalog of
// included and imported themes. The format is documented in documentation/THEMES.md (every key); a sample is in
// documentation/themes/example.moontheme.
//
// Format summary: UTF-8 text (a BOM is allowed), at most 64 KB, one "key = value" per line. Blank lines, lines starting with
// '#' or ';' and an optional "[theme]" header line are ignored. Keys are case-insensitive and must be known (an unknown key
// rejects the file); a repeated key keeps the last value. Colours are #RRGGBB. Numbers are clamped to their range. Booleans are
// true/false/yes/no/on/off/1/0. A missing key inherits from the base theme (Windows 11 Light or Dark).
// Theme files are untrusted input: they hold data only, nothing in them is executed, and the only thing they can name outside
// themselves is a font family (used only if installed).
// Threading: UI thread only.
#pragma once

#include "theme.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace moonsr::app::ui {

constexpr size_t kThemeMaxBytes = 64 * 1024;
constexpr size_t kThemeNameMax = 40;

struct ThemeDef {
    std::wstring name;
    bool baseDark = true;  // `base`: dark (default) or light; everything not given below comes from it
    std::optional<uint32_t> background, surface, border, text, textSecondary, accent, accentText, success, warning, error, info;
    std::wstring font;   // font family (empty = base default; used only when installed)
    int radius = -1;     // maximum corner radius 0..16 (-1 = not given)
    int borderWidth = 1; // 1..3
    bool bracketedControls = false;  // console-style controls: [ON ], [ Start ], block sliders, inverted selection, no icons
    bool promptHeaders = false;      // "[ TITLE ]" section headers, "> Title_" page title, "C:\MoonSR>" status line
    // Logo mark and "SR" gradient (default: derived from `accent`), and the optional effect switches (default: the base
    // theme's: glow on for dark, off for light; scanlines off).
    std::optional<uint32_t> logoStart, logoEnd;
    std::optional<bool> glow, scanlines;
};

// Parses theme text (UTF-8). On failure `error` says what is wrong (with the line number where there is one).
bool ParseThemeText(const std::string& utf8, ThemeDef& out, std::wstring& error);
// Reads a file (size limit enforced before reading) and parses it.
bool ReadThemeFile(const std::wstring& path, ThemeDef& out, std::wstring& error);
// Canonical text for a definition (what import writes and export produces).
std::string SerializeTheme(const ThemeDef& def);
// The palette a definition produces: the base theme's palette with the given tokens applied.
Palette PaletteFromDef(const ThemeDef& def);
// A definition that reproduces a palette (used to export the included themes as templates).
ThemeDef DefFromPalette(const Palette& p, const std::wstring& name);

struct ThemeEntry {
    std::wstring key;  // "light", "dark", "retro" or the custom theme's file stem
    std::wstring name;
    std::wstring description;
    bool builtin = true;
    ThemeDef def;  // custom themes only
    Palette palette;
};

class ThemeCatalog {
public:
    // `dir`: where imported themes live (%LOCALAPPDATA%\MoonSR\themes); created on demand. Empty = no custom themes.
    explicit ThemeCatalog(std::wstring dir);
    void Reload();  // included themes, then every valid *.moontheme in the folder (invalid files are skipped and logged)
    const std::vector<ThemeEntry>& entries() const { return entries_; }
    const ThemeEntry* Find(const std::wstring& key) const;

    enum class ImportMode { Ask, Replace, Rename };
    enum class ImportStatus { Imported, NameExists, Invalid };
    // Validates `path` and stores a normalised copy in the folder. With Ask, a theme of the same name returns NameExists
    // (nothing written; `key` is the existing theme's key); Replace overwrites it; Rename picks a free name ("Name (2)").
    ImportStatus Import(const std::wstring& path, ImportMode mode, std::wstring& error, std::wstring& key);
    bool Remove(const std::wstring& key, std::wstring& error);  // custom themes only
    bool Export(const std::wstring& key, const std::wstring& path, std::wstring& error) const;

private:
    std::wstring FileFor(const std::wstring& key) const;
    std::wstring dir_;
    std::vector<ThemeEntry> entries_;
};

// Parser and catalog self-test (valid / invalid / malformed / oversized files, import, replace, rename, remove) in a scratch
// folder. Returns the number of failed checks; `report` lists every check.
int RunThemeSelfTest(const std::wstring& scratchDir, std::wstring& report);

}  // namespace moonsr::app::ui
