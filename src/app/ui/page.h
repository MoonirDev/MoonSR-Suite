// A scrollable page: a vertical stack of section headers, cards with Settings-style rows, info bars, status tiles,
// a key/value details card and free controls. Layout is computed for a given width; coordinates are page space.
// Threading: UI thread only.
#pragma once

#include "controls.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace moonsr::app::ui {

class View;

struct Row {
    wchar_t icon = 0;
    std::wstring title;
    std::wstring desc;
    // Controls in visual left-to-right order, with their widths (0 = preferred width). Right aligned in the row.
    std::vector<std::pair<Control*, float>> ctrls;
    Control* gate = nullptr;  // row text is dimmed while this control is disabled (default: the first control)
    float height = 0.0f;      // 0 = standard height
    bool hero = false;        // taller row for the primary action
    bool visible = true;      // hidden rows take no space (used for pooled rows such as the model list)
    // layout results
    float y = 0.0f, h = 0.0f;
};

struct Card {
    std::deque<Row> rows;
};

class Details {
public:
    View* view = nullptr;
    std::vector<std::wstring> labels;
    std::vector<std::wstring> values;
    std::vector<char> hidden;  // hidden rows take no space
    void SetValue(size_t i, const std::wstring& v);
    void SetVisible(size_t i, bool visible);
};

class Page {
public:
    Page(View& view, std::wstring title) : view_(view), title_(std::move(title)) {}

    const std::wstring& title() const { return title_; }

    template <class T, class... A>
    T* Make(A&&... args) {
        auto p = std::make_unique<T>(std::forward<A>(args)...);
        T* raw = p.get();
        raw->view = &view_;
        controls_.push_back(std::move(p));
        return raw;
    }

    // Building (in visual order).
    void AddSection(const std::wstring& text);
    Card& AddCard();
    Row& AddRow(Card& card, wchar_t icon, const std::wstring& title, const std::wstring& desc,
                std::vector<std::pair<Control*, float>> ctrls);
    void AddInfo(InfoBar* bar);
    void AddTiles(std::vector<Tile*> tiles);
    Details* AddDetails(std::vector<std::wstring> labels);
    // w = 0: preferred width, w < 0: full page width. h < 0: the control decides (HeightForWidth).
    void AddFree(Control* c, float w, float h);
    // Adds a control to the keyboard / mouse chain without giving it layout space or drawing it; its owner (e.g. the
    // OrderList that positions and draws its per-row combo boxes) sets its bounds and draws it. Call after the owner.
    void AddChild(Control* c) { focusOrder_.push_back(c); }

    // Returns the total content height.
    float Layout(const Gfx& g, float width);
    void Draw(Canvas& c, const Palette& p, const Gfx& g);
    const std::vector<Control*>& focusOrder() const { return focusOrder_; }
    Control* HitTest(float x, float y) const;
    bool Tick(float dt);

private:
    struct Item {
        enum class Kind { Section, Card, Info, Tiles, Details, Free } kind = Kind::Section;
        std::wstring text;
        Card card;
        InfoBar* info = nullptr;
        std::vector<Tile*> tiles;
        Details details;
        Control* free = nullptr;
        float fw = 0, fh = 0;
        float y = 0, h = 0, w = 0;
    };

    void DrawCard(Canvas& c, const Palette& p, const Gfx& g, const Item& it);
    void DrawDetails(Canvas& c, const Palette& p, const Gfx& g, const Item& it);

    View& view_;
    std::wstring title_;
    std::deque<Item> items_;
    std::vector<std::unique_ptr<Control>> controls_;
    std::vector<Control*> focusOrder_;
    float contentWidth_ = 0.0f;
};

}  // namespace moonsr::app::ui
