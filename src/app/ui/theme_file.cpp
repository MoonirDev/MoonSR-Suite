#include "theme_file.h"

#include "core/log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <fstream>
#include <sstream>

namespace moonsr::app::ui {

namespace {

constexpr size_t kMaxLines = 400;
constexpr size_t kMaxLineChars = 300;
constexpr size_t kFontMax = 64;

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) --b;
    return s.substr(a, b - a);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool Utf8ToWide(const std::string& in, std::wstring& out) {
    out.clear();
    if (in.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), out.data(), n) == n;
}

std::string WideToUtf8(const std::wstring& in) {
    if (in.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool ParseColor(const std::wstring& v, uint32_t& out) {
    size_t i = (!v.empty() && v[0] == L'#') ? 1 : 0;
    if (v.size() - i != 6) return false;
    uint32_t r = 0;
    for (; i < v.size(); ++i) {
        const wchar_t c = v[i];
        uint32_t d;
        if (c >= L'0' && c <= L'9') d = static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') d = static_cast<uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') d = static_cast<uint32_t>(c - L'A' + 10);
        else return false;
        r = (r << 4) | d;
    }
    out = r;
    return true;
}

bool ParseInt(const std::wstring& v, int& out) {
    if (v.empty() || v.size() > 6) return false;
    size_t i = 0;
    bool neg = false;
    if (v[0] == L'-') {
        neg = true;
        i = 1;
        if (v.size() == 1) return false;
    }
    int r = 0;
    for (; i < v.size(); ++i) {
        if (v[i] < L'0' || v[i] > L'9') return false;
        r = r * 10 + (v[i] - L'0');
    }
    out = neg ? -r : r;
    return true;
}

bool ParseBool(const std::wstring& v, bool& out) {
    const std::wstring l = Lower(v);
    if (l == L"true" || l == L"yes" || l == L"on" || l == L"1") { out = true; return true; }
    if (l == L"false" || l == L"no" || l == L"off" || l == L"0") { out = false; return true; }
    return false;
}

bool ValidName(const std::wstring& n) {
    if (n.empty() || n.size() > kThemeNameMax) return false;
    for (wchar_t c : n)
        if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF)) return false;
    return true;
}

bool ValidFont(const std::wstring& f) {
    if (f.empty() || f.size() > kFontMax) return false;
    for (wchar_t c : f)
        if (!(std::iswalnum(c) || c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'(' || c == L')')) return false;
    return true;
}

std::wstring ColorText(uint32_t c) { return std::format(L"#{:06X}", c & 0xFFFFFF); }

uint32_t ToRgb(const D2D1_COLOR_F& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

// `c` composited over `under` (straight alpha).
D2D1_COLOR_F Over(const D2D1_COLOR_F& c, const D2D1_COLOR_F& under) {
    return D2D1_COLOR_F{under.r + (c.r - under.r) * c.a, under.g + (c.g - under.g) * c.a, under.b + (c.b - under.b) * c.a, 1.0f};
}

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, a}; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------------------------------

bool ParseThemeText(const std::string& utf8, ThemeDef& out, std::wstring& error) {
    out = ThemeDef{};
    if (utf8.size() > kThemeMaxBytes) {
        error = std::format(L"The file is larger than {} KB.", kThemeMaxBytes / 1024);
        return false;
    }
    if (utf8.find('\0') != std::string::npos) {
        error = L"The file is not a text file (it contains binary data).";
        return false;
    }
    std::string body = utf8;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    std::wstring text;
    if (!Utf8ToWide(body, text)) {
        error = L"The file is not valid UTF-8 text.";
        return false;
    }

    size_t lineNo = 0, pos = 0;
    bool haveName = false;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring raw = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (lineNo > kMaxLines) {
            error = std::format(L"The file has more than {} lines.", kMaxLines);
            return false;
        }
        if (raw.size() > kMaxLineChars) {
            error = std::format(L"Line {} is too long.", lineNo);
            return false;
        }
        const std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line[0] == L'[') {
            if (Lower(line) == L"[theme]") continue;
            error = std::format(L"Line {}: unknown section '{}' (only [theme] is allowed).", lineNo, line.substr(0, 30));
            return false;
        }
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            error = std::format(L"Line {}: expected 'key = value'.", lineNo);
            return false;
        }
        const std::wstring key = Lower(Trim(line.substr(0, eq)));
        const std::wstring val = Trim(line.substr(eq + 1));
        auto bad = [&](const wchar_t* what) {
            error = std::format(L"Line {}: '{}' {}.", lineNo, key, what);
            return false;
        };
        auto colour = [&](std::optional<uint32_t>& dst) {
            uint32_t c = 0;
            if (!ParseColor(val, c)) return bad(L"needs a colour like #1A2B3C");
            dst = c;
            return true;
        };
        if (key == L"name") {
            if (!ValidName(val)) return bad(L"must be 1 to 40 printable characters");
            out.name = val;
            haveName = true;
        } else if (key == L"base") {
            const std::wstring l = Lower(val);
            if (l != L"light" && l != L"dark") return bad(L"must be light or dark");
            out.baseDark = l == L"dark";
        } else if (key == L"background") { if (!colour(out.background)) return false;
        } else if (key == L"surface") { if (!colour(out.surface)) return false;
        } else if (key == L"border") { if (!colour(out.border)) return false;
        } else if (key == L"text") { if (!colour(out.text)) return false;
        } else if (key == L"text_secondary") { if (!colour(out.textSecondary)) return false;
        } else if (key == L"accent") { if (!colour(out.accent)) return false;
        } else if (key == L"accent_text") { if (!colour(out.accentText)) return false;
        } else if (key == L"success") { if (!colour(out.success)) return false;
        } else if (key == L"warning") { if (!colour(out.warning)) return false;
        } else if (key == L"error") { if (!colour(out.error)) return false;
        } else if (key == L"info") { if (!colour(out.info)) return false;
        } else if (key == L"font") {
            if (!val.empty() && !ValidFont(val)) return bad(L"must be a font family name (letters, digits, spaces)");
            out.font = val;
        } else if (key == L"radius") {
            int v = 0;
            if (!ParseInt(val, v)) return bad(L"needs a whole number");
            out.radius = std::clamp(v, 0, 16);
        } else if (key == L"border_width") {
            int v = 0;
            if (!ParseInt(val, v)) return bad(L"needs a whole number");
            out.borderWidth = std::clamp(v, 1, 3);
        } else if (key == L"bracketed_controls") {
            if (!ParseBool(val, out.bracketedControls)) return bad(L"needs true or false");
        } else if (key == L"prompt_headers") {
            if (!ParseBool(val, out.promptHeaders)) return bad(L"needs true or false");
        } else if (key == L"logo_gradient_start") { if (!colour(out.logoStart)) return false;
        } else if (key == L"logo_gradient_end") { if (!colour(out.logoEnd)) return false;
        } else if (key == L"glow" || key == L"scanlines") {
            bool b = false;
            if (!ParseBool(val, b)) return bad(L"needs true or false");
            (key == L"glow" ? out.glow : out.scanlines) = b;
        } else {
            error = std::format(L"Line {}: unknown key '{}'.", lineNo, key.substr(0, 30));
            return false;
        }
    }
    if (!haveName) {
        error = L"The file has no 'name = ...' line.";
        return false;
    }
    return true;
}

bool ReadThemeFile(const std::wstring& path, ThemeDef& out, std::wstring& error) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = L"The file could not be opened.";
        return false;
    }
    const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > kThemeMaxBytes) {
        error = std::format(L"The file is larger than {} KB.", kThemeMaxBytes / 1024);
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = L"The file could not be opened.";
        return false;
    }
    std::string data(static_cast<size_t>(size), '\0');
    f.read(data.data(), static_cast<std::streamsize>(size));
    data.resize(static_cast<size_t>(std::max<std::streamsize>(0, f.gcount())));
    return ParseThemeText(data, out, error);
}

std::string SerializeTheme(const ThemeDef& d) {
    std::wstring t;
    t += L"# MoonSR theme (see documentation/THEMES.md)\n[theme]\n";
    t += L"name = " + d.name + L"\n";
    t += std::wstring(L"base = ") + (d.baseDark ? L"dark" : L"light") + L"\n";
    auto col = [&](const wchar_t* k, const std::optional<uint32_t>& v) {
        if (v) t += std::wstring(k) + L" = " + ColorText(*v) + L"\n";
    };
    col(L"background", d.background);
    col(L"surface", d.surface);
    col(L"border", d.border);
    col(L"text", d.text);
    col(L"text_secondary", d.textSecondary);
    col(L"accent", d.accent);
    col(L"accent_text", d.accentText);
    col(L"success", d.success);
    col(L"warning", d.warning);
    col(L"error", d.error);
    col(L"info", d.info);
    if (!d.font.empty()) t += L"font = " + d.font + L"\n";
    if (d.radius >= 0) t += std::format(L"radius = {}\n", d.radius);
    t += std::format(L"border_width = {}\n", d.borderWidth);
    t += std::wstring(L"bracketed_controls = ") + (d.bracketedControls ? L"true" : L"false") + L"\n";
    t += std::wstring(L"prompt_headers = ") + (d.promptHeaders ? L"true" : L"false") + L"\n";
    col(L"logo_gradient_start", d.logoStart);
    col(L"logo_gradient_end", d.logoEnd);
    if (d.glow) t += std::wstring(L"glow = ") + (*d.glow ? L"true" : L"false") + L"\n";
    if (d.scanlines) t += std::wstring(L"scanlines = ") + (*d.scanlines ? L"true" : L"false") + L"\n";
    return WideToUtf8(t);
}

// ---------------------------------------------------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------------------------------------------------

Palette PaletteFromDef(const ThemeDef& d) {
    Palette p = MakePalette(d.baseDark ? ThemeId::Dark : ThemeId::Light);
    p.name = d.name;
    const D2D1_COLOR_F white = Rgb(0xFFFFFF), black = Rgb(0x000000);
    if (d.background) {
        p.bg = p.bgA = p.bgB = p.bgC = Rgb(*d.background);
        p.micaTint = p.bg;
        p.opaqueBg = true;
        p.backdrop = Backdrop::None;  // a theme with its own background colour is drawn solid
        p.dark = Luma(p.bg) < 0.5f;
    }
    if (d.surface) {
        p.cardFill = Rgb(*d.surface);
        p.popupFill = Rgb(*d.surface);
        p.thumb = Rgb(*d.surface);
    }
    if (d.border) {
        const D2D1_COLOR_F b = Rgb(*d.border);
        p.cardStroke = b;
        p.ctrlStroke = b;
        p.popupStroke = b;
        p.divider = WithAlpha(b, 0.55f);
    }
    if (d.text || d.background) {
        if (d.text) p.text = Rgb(*d.text);
        const D2D1_COLOR_F t = p.text;
        // Control states are translucent washes of the text colour over the surface, so they work on any background.
        p.ctrlFill = WithAlpha(t, 0.07f);
        p.ctrlHover = WithAlpha(t, 0.13f);
        p.ctrlPressed = WithAlpha(t, 0.05f);
        p.ctrlDisabled = WithAlpha(t, 0.04f);
        p.subtleHover = WithAlpha(t, 0.10f);
        p.subtlePressed = WithAlpha(t, 0.06f);
        if (!d.border) {
            p.cardStroke = WithAlpha(t, 0.20f);
            p.ctrlStroke = WithAlpha(t, 0.28f);
            p.popupStroke = WithAlpha(t, 0.30f);
            p.divider = WithAlpha(t, 0.12f);
        }
        if (!d.textSecondary) p.textSecondary = Mix(t, p.bg, 0.35f);
        p.textDisabled = WithAlpha(p.textSecondary, 0.5f);
        p.strong = Mix(t, p.bg, 0.45f);
    }
    if (d.textSecondary) {
        p.textSecondary = Rgb(*d.textSecondary);
        p.textDisabled = WithAlpha(p.textSecondary, 0.5f);
        p.strong = Mix(p.text, p.bg, 0.45f);
    }
    if (d.accent) {
        p.accent = Rgb(*d.accent);
        p.accentHover = Mix(p.accent, p.dark ? white : black, 0.15f);
        p.accentPressed = Mix(p.accent, p.dark ? black : white, 0.15f);
        p.accentDisabled = WithAlpha(p.accent, 0.30f);
        p.gradA = p.gradB = p.brandSR = p.accent;
        p.subtleSelected = WithAlpha(p.accent, 0.22f);
        p.focus = p.accentHover;
        p.textOnAccent = Luma(p.accent) > 0.55f ? black : white;
    }
    if (d.accentText) p.textOnAccent = Rgb(*d.accentText);
    // Console-style controls draw selection as a solid accent block.
    if (d.bracketedControls) p.subtleSelected = p.accent;
    auto status = [&](InfoColors& ic, const std::optional<uint32_t>& c) {
        if (!c) return;
        const D2D1_COLOR_F col = Rgb(*c);
        ic.icon = col;
        ic.fill = Mix(p.bg, col, 0.16f);
        ic.iconText = Luma(col) > 0.55f ? black : white;
    };
    status(p.success, d.success);
    status(p.warning, d.warning);
    status(p.error, d.error);
    status(p.info, d.info);
    if (d.accent && !d.info) {
        p.info.icon = p.accent;
        p.info.fill = Mix(p.bg, p.accent, 0.16f);
        p.info.iconText = p.textOnAccent;
    }
    if (d.bracketedControls) {
        // Console-style info bars are outlined boxes on the plain background.
        p.info.fill = p.success.fill = p.warning.fill = p.error.fill = p.bg;
    }
    p.retro = d.bracketedControls;
    p.promptHeaders = d.promptHeaders;
    p.radius = static_cast<float>(d.radius);
    p.borderWidth = static_cast<float>(d.borderWidth);
    p.font = d.font;
    // Logo: from the explicit keys, else derived from the accent (start = accent, end = the accent turned ~50 degrees
    // towards violet), else the base theme's brand gradient.
    if (d.logoStart || d.logoEnd || d.accent) {
        const D2D1_COLOR_F a = d.logoStart ? Rgb(*d.logoStart) : p.accent;
        const D2D1_COLOR_F b = d.logoEnd ? Rgb(*d.logoEnd) : HueShift(a, 50.0f);
        p.logoA = a;
        p.logoB = b;
        p.logoMid = Mix(a, b, 0.5f);
    }
    if (d.glow) p.glow = *d.glow;
    if (d.scanlines) p.scanlines = *d.scanlines;
    return p;
}

ThemeDef DefFromPalette(const Palette& p, const std::wstring& name) {
    ThemeDef d;
    d.name = name;
    d.baseDark = p.dark;
    const D2D1_COLOR_F bg = p.bg;
    d.background = ToRgb(bg);
    d.surface = ToRgb(Over(p.cardFill, bg));
    d.border = ToRgb(Over(p.cardStroke, bg));
    d.text = ToRgb(p.text);
    d.textSecondary = ToRgb(p.textSecondary);
    d.accent = ToRgb(p.accent);
    d.accentText = ToRgb(p.textOnAccent);
    d.success = ToRgb(p.success.icon);
    d.warning = ToRgb(p.warning.icon);
    d.error = ToRgb(p.error.icon);
    d.info = ToRgb(p.info.icon);
    d.font = p.font;
    d.radius = p.radius < 0.0f ? -1 : static_cast<int>(p.radius + 0.5f);
    d.borderWidth = std::clamp(static_cast<int>(p.borderWidth + 0.5f), 1, 3);
    d.bracketedControls = p.retro;
    d.promptHeaders = p.promptHeaders;
    d.logoStart = ToRgb(p.logoA);
    d.logoEnd = ToRgb(p.logoB);
    d.glow = p.glow;
    d.scanlines = p.scanlines;
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------------------------------------------------

namespace {

std::wstring KeyFromName(const std::wstring& name) {
    std::wstring k;
    for (wchar_t c : name) {
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) k += c;
        else if (c >= L'A' && c <= L'Z') k += static_cast<wchar_t>(c - L'A' + L'a');
        else if (!k.empty() && k.back() != L'-') k += L'-';
    }
    while (!k.empty() && k.back() == L'-') k.pop_back();
    if (k.empty()) k = L"theme";
    if (k.size() > 40) k.resize(40);
    if (k == L"light" || k == L"dark" || k == L"retro" || k == L"retro-amber" || k == L"con" || k == L"nul" || k == L"prn" || k == L"aux") k = L"custom-" + k;
    return k;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

}  // namespace

ThemeCatalog::ThemeCatalog(std::wstring dir) : dir_(std::move(dir)) { Reload(); }

std::wstring ThemeCatalog::FileFor(const std::wstring& key) const { return dir_ + L"\\" + key + L".moontheme"; }

const ThemeEntry* ThemeCatalog::Find(const std::wstring& key) const {
    for (const ThemeEntry& e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

void ThemeCatalog::Reload() {
    entries_.clear();
    auto builtin = [&](const wchar_t* key, const wchar_t* name, const wchar_t* desc, ThemeId id) {
        ThemeEntry e;
        e.key = key;
        e.name = name;
        e.description = desc;
        e.builtin = true;
        e.palette = MakePalette(id);
        e.palette.name = name;
        entries_.push_back(std::move(e));
    };
    builtin(L"light", L"Windows 11 Light", L"Bright surfaces, rounded controls", ThemeId::Light);
    builtin(L"dark", L"Windows 11 Dark", L"Navy surfaces, cyan accent", ThemeId::Dark);
    builtin(L"retro", L"Retro", L"Green on black console, monospace", ThemeId::Retro);
    builtin(L"retro-amber", L"Retro Amber", L"Amber phosphor console, monospace", ThemeId::RetroAmber);
    if (dir_.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir_ + L"\\*.moontheme").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd) && files.size() < 64);
    FindClose(h);
    std::sort(files.begin(), files.end());
    for (const std::wstring& f : files) {
        const std::wstring key = f.substr(0, f.size() - 10);  // strip ".moontheme"
        if (key.empty() || Find(key)) continue;
        ThemeEntry e;
        std::wstring err;
        if (!ReadThemeFile(dir_ + L"\\" + f, e.def, err)) {
            MSR_LOG_WARN(L"Theme file '{}' skipped: {}", f, err);
            continue;
        }
        e.key = key;
        e.name = e.def.name;
        e.builtin = false;
        e.description = std::wstring(L"Custom theme, ") +
                        (e.def.bracketedControls ? L"console style" : e.def.baseDark ? L"based on Windows 11 Dark" : L"based on Windows 11 Light");
        e.palette = PaletteFromDef(e.def);
        entries_.push_back(std::move(e));
    }
}

ThemeCatalog::ImportStatus ThemeCatalog::Import(const std::wstring& path, ImportMode mode, std::wstring& error, std::wstring& key) {
    key.clear();
    ThemeDef def;
    if (!ReadThemeFile(path, def, error)) return ImportStatus::Invalid;
    if (dir_.empty() || (!CreateDirectoryW(dir_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) {
        error = L"The themes folder could not be created.";
        return ImportStatus::Invalid;
    }
    std::wstring k = KeyFromName(def.name);
    // A theme that carries an included theme's name is kept, under a different display name.
    for (const ThemeEntry& e : entries_)
        if (e.builtin && Lower(e.name) == Lower(def.name)) {
            def.name = def.name.substr(0, kThemeNameMax - 9) + L" (custom)";
            k = KeyFromName(def.name);
            break;
        }
    const ThemeEntry* existing = Find(k);
    if (existing && !existing->builtin) {
        if (mode == ImportMode::Ask) {
            key = k;
            return ImportStatus::NameExists;
        }
        if (mode == ImportMode::Rename) {
            const std::wstring baseName = def.name.substr(0, kThemeNameMax - 4);
            for (int n = 2; n < 100; ++n) {
                const std::wstring cand = baseName + std::format(L" ({})", n);
                if (!Find(KeyFromName(cand))) {
                    def.name = cand;
                    k = KeyFromName(cand);
                    break;
                }
            }
            if (Find(k)) {
                error = L"Too many themes with that name.";
                return ImportStatus::Invalid;
            }
        }
    }
    if (!WriteFileUtf8(FileFor(k), SerializeTheme(def))) {
        error = L"The theme could not be saved in the themes folder.";
        return ImportStatus::Invalid;
    }
    Reload();
    key = k;
    if (!Find(k)) {
        error = L"The theme could not be loaded after import.";
        return ImportStatus::Invalid;
    }
    return ImportStatus::Imported;
}

bool ThemeCatalog::Remove(const std::wstring& key, std::wstring& error) {
    const ThemeEntry* e = Find(key);
    if (!e) {
        error = L"No such theme.";
        return false;
    }
    if (e->builtin) {
        error = L"Included themes cannot be removed.";
        return false;
    }
    if (!DeleteFileW(FileFor(key).c_str())) {
        error = L"The theme file could not be deleted.";
        return false;
    }
    Reload();
    return true;
}

bool ThemeCatalog::Export(const std::wstring& key, const std::wstring& path, std::wstring& error) const {
    const ThemeEntry* e = Find(key);
    if (!e) {
        error = L"No such theme.";
        return false;
    }
    ThemeDef d = e->builtin ? DefFromPalette(e->palette, e->name + L" copy") : e->def;
    if (!WriteFileUtf8(path, SerializeTheme(d))) {
        error = L"The file could not be written.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------------------------------------------------

int RunThemeSelfTest(const std::wstring& scratch, std::wstring& report) {
    int failed = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        report += (ok ? L"PASS  " : L"FAIL  ") + what + L"\n";
        if (!ok) ++failed;
    };
    auto parse = [&](const std::string& s, ThemeDef* d = nullptr) {
        ThemeDef tmp;
        std::wstring err;
        const bool ok = ParseThemeText(s, d ? *d : tmp, err);
        return std::pair<bool, std::wstring>(ok, err);
    };

    CreateDirectoryW(scratch.c_str(), nullptr);
    for (const wchar_t* sub : {L"\\themes\\*.moontheme", L"\\src\\*.moontheme"}) {  // start from a clean scratch folder
        WIN32_FIND_DATAW fd{};
        const std::wstring pat = scratch + sub;
        const std::wstring dirOf = pat.substr(0, pat.find_last_of(L'\\') + 1);
        if (HANDLE h = FindFirstFileW(pat.c_str(), &fd); h != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((dirOf + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    ThemeDef d;
    auto r = parse("name = Mini\n", &d);
    check(r.first && d.name == L"Mini" && d.baseDark && !d.accent, L"minimal file (name only) parses; base defaults to dark");
    r = parse("\xEF\xBB\xBF# comment\n[theme]\nNAME = Full theme\nbase = light\nbackground = #FFFFFF\nsurface=F0F0F0\nborder = #AABBCC\n"
              "text = #101010\ntext_secondary = #606060\naccent = #0A84FF\naccent_text = #FFFFFF\nsuccess = #00AA00\nwarning = #FFAA00\n"
              "error = #FF0000\ninfo = #0000FF\nfont = Segoe UI\nradius = 6\nborder_width = 2\nbracketed_controls = yes\nprompt_headers = off\n",
              &d);
    check(r.first && d.name == L"Full theme" && !d.baseDark && d.radius == 6 && d.borderWidth == 2 && d.bracketedControls &&
              !d.promptHeaders && d.surface && *d.surface == 0xF0F0F0,
          L"full file with BOM, comments, section header, mixed case and every key parses");
    r = parse("name = X\nradius = 99\nborder_width = 0\n", &d);
    check(r.first && d.radius == 16 && d.borderWidth == 1, L"numbers are clamped (radius 99 -> 16, border_width 0 -> 1)");
    r = parse("name = X\ncolour = #FFFFFF\n");
    check(!r.first, L"unknown key is rejected: " + r.second);
    r = parse("name = X\naccent = #12345\n");
    check(!r.first, L"short colour is rejected: " + r.second);
    r = parse("name = X\naccent = #GG0000\n");
    check(!r.first, L"non-hex colour is rejected: " + r.second);
    r = parse("name = X\nradius = abc\n");
    check(!r.first, L"non-numeric radius is rejected: " + r.second);
    r = parse("name = X\nbase = grey\n");
    check(!r.first, L"unknown base is rejected: " + r.second);
    r = parse("name = X\nfont = C:\\Windows\\Fonts\\evil.ttf\n");
    check(!r.first, L"font with a path is rejected: " + r.second);
    r = parse("accent = #FFFFFF\n");
    check(!r.first, L"missing name is rejected: " + r.second);
    r = parse("name = X\n[other]\n");
    check(!r.first, L"unknown section is rejected: " + r.second);
    r = parse("name = X\nthis is not a setting\n");
    check(!r.first, L"line without '=' is rejected: " + r.second);
    r = parse(std::string("name = X\n\0\0\x01\x02", 12));
    check(!r.first, L"binary data is rejected: " + r.second);
    r = parse("name = X\n\xFF\xFE\xC0\x80\n");
    check(!r.first, L"invalid UTF-8 is rejected: " + r.second);
    r = parse("name = " + std::string(500, 'a') + "\n");
    check(!r.first, L"over-long line is rejected: " + r.second);
    r = parse("name = X\n" + std::string(kThemeMaxBytes + 10, '#'));
    check(!r.first, L"oversized text is rejected: " + r.second);
    r = parse("name = \x01" "bad\n");
    check(!r.first, L"control character in name is rejected: " + r.second);
    r = parse("");
    check(!r.first, L"empty file is rejected: " + r.second);
    {
        std::string lines = "name = X\n";
        for (int i = 0; i < 500; ++i) lines += "# filler\n";
        r = parse(lines);
        check(!r.first, L"too many lines is rejected: " + r.second);
    }
    {  // palette
        ThemeDef pd;
        parse("name = P\nbase = dark\nbackground = #101820\ntext = #E0E0E0\naccent = #FFB000\nradius = 0\n", &pd);
        const Palette p = PaletteFromDef(pd);
        check(p.opaqueBg && p.radius == 0.0f && std::abs(p.accent.r - 1.0f) < 0.01f && std::abs(p.bg.b - 0x20 / 255.0f) < 0.01f,
              L"palette applies the given tokens and inherits the rest");
    }
    {  // logo and effect keys
        ThemeDef ld;
        r = parse("name = L\naccent = #FF8000\nlogo_gradient_start = #00FF00\nlogo_gradient_end = #0000FF\nglow = off\nscanlines = yes\n", &ld);
        const Palette lp = PaletteFromDef(ld);
        check(r.first && ld.logoStart == 0x00FF00u && ld.logoEnd == 0x0000FFu && ld.glow == false && ld.scanlines == true &&
                  lp.logoA.g > 0.99f && lp.logoB.b > 0.99f && !lp.glow && lp.scanlines,
              L"logo_gradient_start / _end, glow and scanlines parse and apply");
        ThemeDef dd;
        parse("name = D\naccent = #FF8000\n", &dd);
        const Palette dp = PaletteFromDef(dd);
        check(std::abs(dp.logoA.r - 1.0f) < 0.01f && std::abs(dp.logoB.r - dp.logoA.r) + std::abs(dp.logoB.g - dp.logoA.g) +
                  std::abs(dp.logoB.b - dp.logoA.b) > 0.2f && dp.glow && !dp.scanlines,
              L"logo gradient is derived from the accent; effects inherit the base theme");
        r = parse("name = X\nglow = maybe\n");
        check(!r.first, L"non-boolean glow is rejected: " + r.second);
        ThemeDef rt;
        const bool rok = parse(SerializeTheme(ld), &rt).first;
        check(rok && rt.logoStart == ld.logoStart && rt.logoEnd == ld.logoEnd && rt.glow == ld.glow && rt.scanlines == ld.scanlines,
              L"logo and effect keys survive a round trip");
    }
    {  // round trip
        ThemeDef a;
        parse("name = Round trip\nbase = light\naccent = #123456\nradius = 3\nprompt_headers = true\n", &a);
        ThemeDef b;
        const bool ok = parse(SerializeTheme(a), &b).first;
        check(ok && b.name == a.name && b.baseDark == a.baseDark && b.accent == a.accent && b.radius == 3 && b.promptHeaders,
              L"serialised theme parses back to the same definition");
    }
    // Catalog: import, same name, replace, rename, remove, files on disk.
    const std::wstring dir = scratch + L"\\themes";
    const std::wstring src = scratch + L"\\src";
    CreateDirectoryW(src.c_str(), nullptr);
    {
        ThemeCatalog cat(dir);
        check(cat.entries().size() == 4 && cat.Find(L"light") && cat.Find(L"dark") && cat.Find(L"retro") && cat.Find(L"retro-amber") &&
                  cat.Find(L"retro-amber")->builtin && cat.Find(L"retro-amber")->palette.retro,
              L"catalog starts with the four included themes (Retro Amber included)");
        {
            const Palette& am = cat.Find(L"retro-amber")->palette;
            const Palette& gr = cat.Find(L"retro")->palette;
            check(am.promptHeaders && am.scanlines && am.glow && am.backdrop == Backdrop::Acrylic && am.accent.r > 0.9f && am.accent.g < 0.7f &&
                      am.accent.b < 0.2f && gr.accent.g > gr.accent.r,
                  L"Retro Amber copies Retro (console, glow, scanlines, acrylic) with an orange accent");
            const float dw = std::abs(am.warning.icon.b - am.accent.b) + std::abs(am.warning.icon.g - am.accent.g);
            check(dw > 0.4f && am.error.icon.r > 0.9f && am.error.icon.g < 0.4f, L"Retro Amber warning and error colours differ from the accent");
        }
        const std::wstring f1 = src + L"\\a.moontheme";
        WriteFileUtf8(f1, "name = Sunset\nbase = dark\naccent = #FF8040\n");
        std::wstring err, key;
        auto st = cat.Import(f1, ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::Imported && key == L"sunset" && cat.Find(L"sunset") && !cat.Find(L"sunset")->builtin,
              L"import adds a custom theme");
        st = cat.Import(f1, ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::NameExists && key == L"sunset", L"same name asks before replacing");
        st = cat.Import(f1, ThemeCatalog::ImportMode::Rename, err, key);
        check(st == ThemeCatalog::ImportStatus::Imported && key == L"sunset-2" && cat.Find(L"sunset-2") &&
                  cat.Find(L"sunset-2")->name == L"Sunset (2)",
              L"same name can be kept under a new name");
        WriteFileUtf8(f1, "name = Sunset\naccent = #00FF00\n");
        st = cat.Import(f1, ThemeCatalog::ImportMode::Replace, err, key);
        check(st == ThemeCatalog::ImportStatus::Imported && cat.Find(L"sunset") && cat.Find(L"sunset")->def.accent == 0x00FF00u &&
                  cat.entries().size() == 6,
              L"same name can replace the existing theme");
        const std::wstring f2 = src + L"\\bad.moontheme";
        WriteFileUtf8(f2, "name = Bad\naccent = red\n");
        const size_t before = cat.entries().size();
        st = cat.Import(f2, ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::Invalid && !err.empty() && cat.entries().size() == before &&
                  GetFileAttributesW((dir + L"\\bad.moontheme").c_str()) == INVALID_FILE_ATTRIBUTES,
              L"invalid file imports nothing: " + err);
        WriteFileUtf8(f2, std::string(kThemeMaxBytes + 1, 'x'));
        st = cat.Import(f2, ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::Invalid, L"oversized file on disk is rejected before reading: " + err);
        st = cat.Import(src + L"\\missing.moontheme", ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::Invalid, L"missing file is rejected: " + err);
        WriteFileUtf8(f2, "name = Retro\naccent = #FF0000\n");
        st = cat.Import(f2, ThemeCatalog::ImportMode::Ask, err, key);
        check(st == ThemeCatalog::ImportStatus::Imported && cat.Find(key) && !cat.Find(key)->builtin && cat.Find(L"retro")->builtin &&
                  cat.Find(key)->name != L"Retro",
              L"a theme named like an included one is kept without replacing it");
        const bool removedBuiltin = cat.Remove(L"dark", err);
        check(!removedBuiltin && cat.Find(L"dark"), L"included themes cannot be removed: " + err);
        check(cat.Remove(L"sunset-2", err) && !cat.Find(L"sunset-2"), L"custom theme can be removed");
        const std::wstring ex = scratch + L"\\export.moontheme";
        check(cat.Export(L"retro", ex, err), L"included theme can be exported as a template");
        ThemeDef exd;
        std::wstring perr;
        check(ReadThemeFile(ex, exd, perr) && exd.bracketedControls && exd.promptHeaders, L"exported template parses again: " + perr);
    }
    {
        ThemeCatalog again(dir);
        check(again.Find(L"sunset") != nullptr, L"imported theme is found again after a reload");
    }
    return failed;
}

}  // namespace moonsr::app::ui
