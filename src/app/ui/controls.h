// Custom-drawn Fluent controls. Every control draws itself with a Canvas and reacts to mouse and keyboard input
// routed by View; `bounds` is in the coordinate space of the owner (page space for page controls, window space for
// navigation items). Threading: UI thread only.
#pragma once

#include "gfx.h"
#include "theme.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace moonsr::app::ui {

class View;

class Control {
public:
    virtual ~Control() = default;

    Rect bounds{};
    bool visible = true;
    bool enabled = true;
    bool focusable = true;
    bool interactive = true;  // false: never hovered or pressed (info bars, tiles)
    bool hover = false;
    bool pressed = false;
    bool focused = false;
    bool focusVisible = false;  // keyboard focus ring is drawn only after keyboard navigation
    // Animated hover / press amounts (0..1) behind the smooth hover and press fades. They follow `hover` / `pressed`
    // (StepState); with visual effects off they snap.
    float hoverT = 0.0f, pressT = 0.0f;
    View* view = nullptr;
    std::function<void()> onChange;  // fired on user-driven changes (click for buttons)

    virtual void Draw(Canvas& c, const Palette& p) = 0;
    virtual float PreferredWidth(const Gfx&) const { return 0.0f; }
    virtual float PreferredHeight() const { return metric::kControlHeight; }
    virtual void OnPress(float x, float y) { (void)x; (void)y; }
    virtual void OnDrag(float x, float y) { (void)x; (void)y; }
    virtual void OnRelease(float x, float y, bool inside) { (void)x; (void)y; (void)inside; }
    virtual bool OnKey(UINT vk) { (void)vk; return false; }  // arrow keys etc.; true = consumed
    virtual void Activate() {}                               // Space / Enter
    virtual bool UsesEnter() const { return false; }         // Enter activates it (else Enter is the page default)
    virtual bool Tick(float dt) { (void)dt; return false; }  // animation step; true while still animating
    virtual bool IsLink() const { return false; }
    virtual float HeightForWidth(const Gfx& g, float width) const { (void)g; (void)width; return PreferredHeight(); }
    virtual bool OnAltKey(UINT vk) { (void)vk; return false; }  // Alt+arrow while focused; true = consumed
    virtual bool OnEscape() { return false; }                   // Esc cancels a drag in progress; true = consumed
    virtual void OnHoverMove(float x, float y) { (void)x; (void)y; }  // mouse moved over the control (no button down)
    virtual bool OnChar(wchar_t ch) { (void)ch; return false; }        // typed character (text entry); true = consumed
    virtual void OnFocusChanged(bool gained, bool byKeyboard) { (void)gained; (void)byKeyboard; }
    // Mouse wheel over the control (delta in WHEEL_DELTA units); true = consumed (it scrolled), false = let the page scroll.
    virtual bool OnWheel(float delta) { (void)delta; return false; }
    // True while an animation of this control has not reached its target (the view keeps its timer running).
    virtual bool AnimPending() const;

    // Moves hoverT / pressT towards their targets. `animate` false snaps them. `afterglow`: console phosphor, lights up at
    // once and fades out slowly (a short trail when the highlight moves). Returns true while still moving.
    bool StepState(float dt, bool animate, bool afterglow);

    void SetEnabled(bool e);
    void SetVisible(bool v);
    void Invalidate();

protected:
    void DrawFocus(Canvas& c, const Palette& p, const Rect& r, float radius, float outset = 3.0f) const;
    // `normal` blended towards `hov` by hoverT and then towards `prs` by pressT (alpha-aware, so transparent fills fade).
    D2D1_COLOR_F StateColor(const D2D1_COLOR_F& normal, const D2D1_COLOR_F& hov, const D2D1_COLOR_F& prs) const;
    float Lit() const { return enabled ? std::max(hoverT, pressT) : 0.0f; }  // console inverted-highlight strength
    bool Effects() const;     // visual effects on (animations allowed)
    bool CursorOn() const;    // blinking cursors: currently in the visible phase (always true with effects off)
};

// Alpha-aware colour blend: rgb weighted by alpha, so a fade from transparent does not pass through black.
D2D1_COLOR_F BlendOver(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);

class ToggleSwitch : public Control {
public:
    bool checked() const { return checked_; }
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    void SetChecked(bool v, bool animate = false);
    void Draw(Canvas& c, const Palette& p) override;
    void OnRelease(float x, float y, bool inside) override;
    void Activate() override;
    bool Tick(float dt) override;
    bool AnimPending() const override;

private:
    void Toggle();
    bool checked_ = false;
    float t_ = 0.0f;
};

class Button : public Control {
public:
    std::wstring text;
    bool accent = false;
    float height = metric::kControlHeight;
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    float RetroPreferredWidth(const Gfx& g) const;
    float PreferredHeight() const override { return height; }
    void SetText(const std::wstring& t);
    void Draw(Canvas& c, const Palette& p) override;
    float PreferredWidth(const Gfx& g) const override;
    void OnRelease(float x, float y, bool inside) override;
    void Activate() override;
    bool UsesEnter() const override { return true; }
};

class LinkButton : public Control {
public:
    std::wstring text;
    void Draw(Canvas& c, const Palette& p) override;
    float PreferredWidth(const Gfx& g) const override;
    float PreferredHeight() const override { return 24.0f; }
    void OnRelease(float x, float y, bool inside) override;
    void Activate() override;
    bool UsesEnter() const override { return true; }
    bool IsLink() const override { return true; }
};

class ComboBox : public Control {
public:
    struct Item {
        std::wstring text;
        long long data = 0;
    };
    void Clear();
    int AddItem(const std::wstring& text, long long data = 0);
    int Count() const { return static_cast<int>(items_.size()); }
    int Selected() const { return selected_; }
    void SetSelected(int index, bool notify = false);
    long long ItemData(int index) const;
    const std::vector<Item>& items() const { return items_; }
    void SetOpen(bool o) { open_ = o; Invalidate(); }
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp

    void Draw(Canvas& c, const Palette& p) override;
    void OnRelease(float x, float y, bool inside) override;
    void Activate() override;
    bool OnKey(UINT vk) override;
    bool UsesEnter() const override { return true; }

private:
    std::vector<Item> items_;
    int selected_ = -1;
    bool open_ = false;
};

class Slider : public Control {
public:
    // Integer range with a step; the label shows value + suffix.
    void Setup(int minV, int maxV, int step, const wchar_t* suffix);
    // Optional fixed-point label; values and steps remain integers.
    void SetDisplayDivisor(int divisor) { displayDivisor_ = divisor > 1 ? divisor : 1; }
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    int value() const { return value_; }
    void SetValue(int v, bool notify = false);
    void Draw(Canvas& c, const Palette& p) override;
    void OnPress(float x, float y) override;
    void OnDrag(float x, float y) override;
    bool OnKey(UINT vk) override;
    bool Tick(float dt) override;
    bool AnimPending() const override;

private:
    float TargetT() const { return max_ > min_ ? static_cast<float>(value_ - min_) / static_cast<float>(max_ - min_) : 0.0f; }
    float ShownT();  // eased thumb position (follows the value; jumps while dragging or with effects off)
    float shown_ = -1.0f;
    void SetFromX(float x);
    float TrackLeft() const { return bounds.left + 10.0f; }
    float TrackRight() const { return bounds.right - (displayDivisor_ > 1 ? 72.0f : 58.0f); }
    int min_ = 0, max_ = 100, step_ = 1, value_ = 0;
    int displayDivisor_ = 1;
    std::wstring suffix_;
};

// Single-line integer entry. Digits, Backspace / Delete, Left / Right / Home / End (Shift extends the selection), Ctrl+A / C /
// X / V (pasted text is reduced to its digits), Up / Down step by one. The value is committed on Enter and when focus
// leaves; out-of-range input is clamped and flagged (clamped()), empty input reverts. Threading: UI thread only.
class NumberBox : public Control {
public:
    void Setup(int minV, int maxV, const wchar_t* suffix);
    int value() const { return value_; }
    void SetValue(int v, bool notify = false);
    bool clamped() const { return clamped_; }  // the last commit clamped the typed number (cleared by the next edit)
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    int lastTyped() const { return typed_; }
    const std::wstring& text() const { return text_; }

    void Draw(Canvas& c, const Palette& p) override;
    void OnPress(float x, float y) override;
    void OnDrag(float x, float y) override;
    bool OnKey(UINT vk) override;
    bool OnChar(wchar_t ch) override;
    void OnFocusChanged(bool gained, bool byKeyboard) override;
    void Activate() override { Commit(true); }
    bool UsesEnter() const override { return true; }
    float PreferredWidth(const Gfx&) const override { return 160.0f; }

private:
    void Commit(bool notify);
    void Replace(const std::wstring& s);  // replaces the selection
    int CaretFromX(float x) const;
    float XOf(int index) const;
    bool HasSelection() const { return caret_ != anchor_; }
    int min_ = 0, max_ = 100, value_ = 0, typed_ = 0;
    std::wstring suffix_, text_ = L"0";
    int caret_ = 0, anchor_ = 0;
    bool clamped_ = false;
    const Gfx* gfx_ = nullptr;  // for text measurement while handling mouse input
};

class InfoBar : public Control {
public:
    enum class Severity { Info, Success, Warning, Error };
    InfoBar() { focusable = false; interactive = true; }
    void Set(Severity s, const std::wstring& message);
    // Closed with its x button; the same text stays hidden for this run, a new message shows again.
    bool Dismissed() const;
    void OnRelease(float x, float y, bool inside) override;
    bool Tick(float dt) override;
    bool AnimPending() const override;
    Rect CloseRect() const { return Rect{bounds.right - 34.0f, bounds.top + 6.0f, bounds.right - 6.0f, bounds.top + 34.0f}; }
    const std::wstring& message() const { return message_; }
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    float RetroMeasure(const Gfx& g, float width) const;
    float Measure(const Gfx& g, float width) const;
    void Draw(Canvas& c, const Palette& p) override;

private:
    Severity severity_ = Severity::Info;
    std::wstring message_;
    bool closing_ = false;
    float shown_ = 1.0f;
    mutable float fullH_ = 0.0f;  // height before closing, for drawing the shrinking bar
    float Ease() const;
    void FinishClose();
};

// Label + large value, optionally with a small unit / note after the value.
class Tile : public Control {
public:
    Tile() { focusable = false; interactive = false; }
    std::wstring label;
    void SetValue(const std::wstring& value, const std::wstring& note = L"");
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    void Draw(Canvas& c, const Palette& p) override;

private:
    std::wstring value_ = L"-";
    std::wstring note_;
};

// Static right-aligned text, optionally drawn as a keyboard key cap.
class TextLabel : public Control {
public:
    TextLabel() { focusable = false; interactive = false; }
    std::wstring text;
    bool keycap = false;
    void SetText(const std::wstring& t);
    void Draw(Canvas& c, const Palette& p) override;
    float PreferredWidth(const Gfx& g) const override;
};

// A wrapped paragraph on a card, optionally with a bold title. Static; height follows the text.
class TextBlock : public Control {
public:
    TextBlock() { focusable = false; interactive = false; }
    std::wstring title;
    std::wstring text;
    void SetText(const std::wstring& t);
    void Draw(Canvas& c, const Palette& p) override;
    float HeightForWidth(const Gfx& g, float width) const override;
};

// The "load order" list: one row per stage with a position number, icon, name, description, state badges and
// move up / move down buttons and, optionally, a combo box per stage (Entry::combo). Rows can be dragged (insertion line shown while dragging, Esc cancels) or moved
// with Alt+Up / Alt+Down while the list has keyboard focus (Up / Down select a row). The caller owns the meaning of
// `id`; the list only keeps the order and reports it through onChange. Threading: UI thread only.
class OrderList : public Control {
public:
    struct Entry {
        int id = 0;
        wchar_t icon = 0;
        std::wstring name, desc;
        std::wstring badge;  // state ("On" / "Off")
        bool badgeOn = false;
        std::wstring note;   // optional second badge (warning colour), e.g. "Not active yet"
        ComboBox* combo = nullptr;  // optional per-stage combo box (owned by the page); the list positions and draws it
                                    // in this row, so it moves with the stage when the order changes
    };
    static constexpr float kRowH = 72.0f;
    static constexpr float kComboGap = 12.0f;

    std::vector<Entry> entries;  // current order, top to bottom
    void SetBadge(int id, const std::wstring& badge, bool on, const std::wstring& note);
    bool MoveEntry(int from, int to);  // reorders, animates, fires onChange
    int Selected() const { return sel_; }
    bool dragging() const { return dragging_; }

    void Draw(Canvas& c, const Palette& p) override;
    float PreferredHeight() const override { return kRowH * static_cast<float>(entries.size()); }
    float HeightForWidth(const Gfx&, float) const override { return PreferredHeight(); }
    void OnPress(float x, float y) override;
    void OnDrag(float x, float y) override;
    void OnRelease(float x, float y, bool inside) override;
    bool OnKey(UINT vk) override;
    bool OnAltKey(UINT vk) override;
    bool OnEscape() override;
    void OnHoverMove(float x, float y) override;
    bool Tick(float dt) override;

private:
    enum class Hit { None, Row, Up, Down };
    Hit HitAt(float x, float y, int* row) const;  // x, y local to the control
    Rect UpRect(int row) const;
    Rect DownRect(int row) const;
    int DropIndex() const;
    float ComboWidth() const;
    Rect ComboRect(float rowTop) const;  // where an entry's combo box sits in a row whose top edge is rowTop
    void SyncCombos();                   // puts every combo at its resting position (hit testing uses these)

    int sel_ = 0;
    Hit pressHit_ = Hit::None;
    int pressRow_ = -1;
    float pressY_ = 0, dragY_ = 0;  // local coordinates
    bool dragging_ = false, cancelled_ = false;
    Hit hoverHit_ = Hit::None;
    int hoverRow_ = -1;
    std::vector<std::pair<int, float>> anim_;  // id -> remaining vertical offset (px), decays to zero
};

// The Home page hero: navy gradient card with the logo mark and its glow, the wordmark, the tag line and the gradient rule
// (assets/brand/moonsr_logo.svg). It keeps the brand colours in every Fluent theme. Static (not focusable).
class HeroBanner : public Control {
public:
    HeroBanner() { focusable = false; interactive = false; }
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    static float RetroHeight();
    void Draw(Canvas& c, const Palette& p) override;
    float HeightForWidth(const Gfx& g, float width) const override;

private:
    static float ScaleFor(float width);
};

class NavItem : public Control {
public:
    wchar_t icon = 0;
    std::wstring label;
    bool selected = false;
    bool compact = false;
    int index = 0;
    void DrawRetro(Canvas& c, const Palette& p);  // console theme, retro_controls.cpp
    void Draw(Canvas& c, const Palette& p) override;
    void OnRelease(float x, float y, bool inside) override;
    void Activate() override;
    bool OnKey(UINT vk) override;
    bool UsesEnter() const override { return true; }
    bool Tick(float dt) override;
    bool AnimPending() const override;
    float selT = 0.0f;  // animated selection highlight (console afterglow when the selection moves)
};

// Theme picker: one card per theme (included and imported) with a live miniature drawn from that theme's own palette (nav, a
// card with a toggle and a slider), the selected one marked. Click or Up / Down selects and fires onChange. Imported themes
// show a Remove action (onRemove; Delete key too); included ones have none. The cards sit in a scroll region of about two
// and a half cards with its own themed scrollbar (wheel, thumb drag, track click; keyboard selection scrolls the selected
// card into view). Threading: UI thread only.
class ThemePicker : public Control {
public:
    struct Card {
        std::wstring key, name, desc;
        bool builtin = true;
        Palette palette;
    };
    std::function<void(const std::wstring&)> onRemove;
    void SetCards(std::vector<Card> cards);  // relayouts
    const std::wstring& selectedKey() const { return sel_; }
    void SetSelected(const std::wstring& key);  // no notification
    void Draw(Canvas& c, const Palette& p) override;
    void OnPress(float x, float y) override;
    void OnRelease(float x, float y, bool inside) override;
    void OnHoverMove(float x, float y) override;
    void OnDrag(float x, float y) override;
    bool OnKey(UINT vk) override;
    bool OnWheel(float delta) override;
    void Activate() override;
    bool Tick(float dt) override;
    bool AnimPending() const override;
    float PreferredHeight() const override { return 340.0f; }
    float HeightForWidth(const Gfx& g, float width) const override;
    float scroll() const { return scroll_; }
    void SetScroll(float v) { ScrollTo(v, false); }  // tests

private:
    float ContentHeight() const;
    float MaxScroll() const;
    bool Scrollable() const { return MaxScroll() > 0.5f; }
    Rect TrackRect() const;
    Rect ThumbRect() const;
    void ScrollTo(float v, bool animate);
    void EnsureVisible(int i, bool animate);
    void DrawScrollbar(Canvas& c, const Palette& p);
    float scroll_ = 0.0f, scrollTarget_ = 0.0f;
    bool dragThumb_ = false, hoverBar_ = false;
    bool ensurePending_ = false;  // scroll the selected card into view once the control has been laid out
    float dragOff_ = 0.0f;
    std::vector<float> cardHover_;
    Rect CardRect(int i) const;
    Rect RemoveRect(int i) const;
    int CardAt(float x, float y) const;
    int SelectedIndex() const;
    void Select(int i, bool notify);
    static void DrawMiniature(Canvas& c, const Rect& r, const Palette& pal);
    std::vector<Card> cards_;
    std::wstring sel_ = L"dark";
    int hoverCard_ = -1, pressCard_ = -1;
    bool hoverRemove_ = false, pressRemove_ = false;
};

}  // namespace moonsr::app::ui
