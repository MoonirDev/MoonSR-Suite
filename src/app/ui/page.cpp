#include "page.h"

#include "view.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace moonsr::app::ui {

namespace {
constexpr float kRowPadX = 16.0f;
constexpr float kTileMinWidth = 172.0f;
constexpr float kTileHeight = 88.0f;
constexpr float kTileGap = 8.0f;
constexpr float kDetailLineHeight = 40.0f;
constexpr float kSectionHeight = 44.0f;
}  // namespace

void Details::SetValue(size_t i, const std::wstring& v) {
    if (i >= values.size() || values[i] == v) return;
    values[i] = v;
    if (view) view->Invalidate();
}

void Details::SetVisible(size_t i, bool visible) {
    if (i >= hidden.size() || (hidden[i] == 0) == visible) return;
    hidden[i] = visible ? 0 : 1;
    if (view) {
        view->RequestLayout();
        view->Invalidate();
    }
}

void Page::AddSection(const std::wstring& text) {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Section;
    it.text = text;
}

Card& Page::AddCard() {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Card;
    return it.card;
}

Row& Page::AddRow(Card& card, wchar_t icon, const std::wstring& title, const std::wstring& desc,
                  std::vector<std::pair<Control*, float>> ctrls) {
    Row& r = card.rows.emplace_back();
    r.icon = icon;
    r.title = title;
    r.desc = desc;
    r.ctrls = std::move(ctrls);
    for (auto& pc : r.ctrls) focusOrder_.push_back(pc.first);
    if (!r.ctrls.empty()) r.gate = r.ctrls.front().first;
    return r;
}

void Page::AddInfo(InfoBar* bar) {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Info;
    it.info = bar;
}

void Page::AddTiles(std::vector<Tile*> tiles) {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Tiles;
    it.tiles = std::move(tiles);
}

Details* Page::AddDetails(std::vector<std::wstring> labels) {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Details;
    it.details.view = &view_;
    it.details.values.assign(labels.size(), L"-");
    it.details.hidden.assign(labels.size(), 0);
    it.details.labels = std::move(labels);
    return &it.details;
}

void Page::AddFree(Control* c, float w, float h) {
    Item& it = items_.emplace_back();
    it.kind = Item::Kind::Free;
    it.free = c;
    it.fw = w;
    it.fh = h;
    focusOrder_.push_back(c);
}

float Page::Layout(const Gfx& g, float width) {
    contentWidth_ = width;
    float y = 0.0f;
    const bool icons = g.hasIcons();
    for (Item& it : items_) {
        it.y = y;
        it.w = width;
        switch (it.kind) {
        case Item::Kind::Section:
            it.h = kSectionHeight;
            y += it.h;
            break;
        case Item::Kind::Info: {
            if (!it.info->visible) {
                it.h = 0.0f;
                break;
            }
            it.h = it.info->Measure(g, width);
            it.info->bounds = Rect{0, y, width, y + it.h};
            y += it.h + metric::kItemGap;
            break;
        }
        case Item::Kind::Tiles: {
            const int n = static_cast<int>(it.tiles.size());
            const int cols = std::max(1, static_cast<int>(std::floor((width + kTileGap) / (kTileMinWidth + kTileGap))));
            const float tw = (width - kTileGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            for (int i = 0; i < n; ++i) {
                const float tx = static_cast<float>(i % cols) * (tw + kTileGap);
                const float ty = y + static_cast<float>(i / cols) * (kTileHeight + kTileGap);
                it.tiles[static_cast<size_t>(i)]->bounds = Rect{tx, ty, tx + tw, ty + kTileHeight};
            }
            const int rows = (n + cols - 1) / cols;
            it.h = static_cast<float>(rows) * (kTileHeight + kTileGap) - kTileGap;
            y += it.h + metric::kItemGap;
            break;
        }
        case Item::Kind::Details:
            {
                size_t shown = 0;
                for (char h : it.details.hidden) shown += h ? 0 : 1;
                it.h = static_cast<float>(shown) * kDetailLineHeight;
            }
            y += it.h + metric::kItemGap;
            break;
        case Item::Kind::Free: {
            if (!it.free->visible) {
                it.h = 0.0f;
                break;
            }
            const float w = it.fw < 0.0f ? width : it.fw > 0.0f ? it.fw : it.free->PreferredWidth(g);
            it.h = it.fh < 0.0f ? it.free->HeightForWidth(g, w) : it.fh;
            it.free->bounds = Rect{0, y, w, y + it.h};
            y += it.h + metric::kItemGap;
            break;
        }
        case Item::Kind::Card: {
            float ry = 0.0f;
            for (Row& r : it.card.rows) {
                r.y = ry;
                if (!r.visible) {
                    r.h = 0.0f;
                    continue;
                }
                r.h = r.height > 0.0f ? r.height : (r.desc.empty() ? metric::kRowHeight : metric::kRowHeightDesc);
                // Control widths; shrink the widest combo box when the text column would get too narrow.
                std::vector<float> ws;
                float total = 0.0f;
                for (const auto& pc : r.ctrls) {
                    float cw = pc.second > 0.0f ? pc.second : pc.first->PreferredWidth(g);
                    // Monospace text is wider: give fixed-width combo boxes and buttons more room in the console theme.
                    if (g.retro() && pc.second > 0.0f && (dynamic_cast<ComboBox*>(pc.first) || dynamic_cast<Button*>(pc.first))) cw *= 1.12f;
                    ws.push_back(cw);
                    total += ws.back() + 8.0f;
                }
                const float textLeft = kRowPadX + ((icons && r.icon != 0) ? 40.0f : 0.0f);
                const float deficit = 170.0f - (width - kRowPadX - total - 16.0f - textLeft);
                if (deficit > 0.0f && !ws.empty()) {
                    size_t widest = 0;
                    for (size_t i = 1; i < ws.size(); ++i)
                        if (ws[i] > ws[widest]) widest = i;
                    if (dynamic_cast<ComboBox*>(r.ctrls[widest].first) || dynamic_cast<Slider*>(r.ctrls[widest].first))
                        ws[widest] = std::max(150.0f, ws[widest] - deficit);
                }
                float x = width - kRowPadX;
                for (size_t i = r.ctrls.size(); i-- > 0;) {
                    Control* c = r.ctrls[i].first;
                    const float cw = ws[i];
                    const float ch = c->PreferredHeight();
                    const float cy = it.y + ry + r.h * 0.5f;
                    c->bounds = Rect{x - cw, cy - ch * 0.5f, x, cy + ch * 0.5f};
                    x -= cw + 8.0f;
                }
                ry += r.h;
            }
            it.h = ry;
            if (it.h > 0.0f) y += it.h + metric::kItemGap;
            break;
        }
        }
    }
    // Rows reference absolute control positions computed with it.y; nothing else to do.
    (void)icons;
    return std::max(0.0f, y - metric::kItemGap) + 24.0f;
}

void Page::DrawCard(Canvas& c, const Palette& p, const Gfx& g, const Item& it) {
    if (it.h <= 0.0f) return;
    const Rect card{0, it.y, it.w, it.y + it.h};
    c.FillRound(card, metric::kCardRadius, p.cardFill);
    c.StrokeRound(card, metric::kCardRadius, p.cardStroke);
    const bool icons = g.hasIcons();
    bool first = true;
    for (const Row& r : it.card.rows) {
        if (!r.visible) continue;
        const float top = it.y + r.y;
        if (!first) c.Line(1.0f, top, it.w - 1.0f, top, p.divider, 1.0f);
        first = false;
        const bool on = !r.gate || r.gate->enabled;
        const D2D1_COLOR_F tc = on ? p.text : p.textDisabled;
        const D2D1_COLOR_F sc = on ? p.textSecondary : p.textDisabled;
        float tx = kRowPadX;
        if (icons && r.icon != 0) {
            c.Icon(r.icon, Rect{kRowPadX, top + r.h * 0.5f - 12.0f, kRowPadX + 24.0f, top + r.h * 0.5f + 12.0f}, Font::Icon, tc);
            tx = kRowPadX + 24.0f + 16.0f;
        }
        float right = it.w - kRowPadX;
        for (const auto& pc : r.ctrls) right = std::min(right, pc.first->bounds.left);
        right -= 16.0f;
        if (r.desc.empty()) {
            c.Text(r.title, Rect{tx, top, right, top + r.h}, Font::Body, tc);
        } else {
            const float blockH = 20.0f + 18.0f;
            const float by = top + (r.h - blockH) * 0.5f;
            c.Text(r.title, Rect{tx, by, right, by + 20.0f}, r.hero ? Font::BodyStrong : Font::Body, tc);
            c.Text(r.desc, Rect{tx, by + 20.0f, right, by + 20.0f + 18.0f}, Font::Caption, sc);
        }
    }
    for (const Row& r : it.card.rows)
        if (r.visible)
            for (const auto& pc : r.ctrls)
                if (pc.first->visible) pc.first->Draw(c, p);
}

void Page::DrawDetails(Canvas& c, const Palette& p, const Gfx& g, const Item& it) {
    const Rect card{0, it.y, it.w, it.y + it.h};
    c.FillRound(card, metric::kCardRadius, p.cardFill);
    c.StrokeRound(card, metric::kCardRadius, p.cardStroke);
    float labelW = 0.0f;
    for (const auto& l : it.details.labels) labelW = std::max(labelW, g.MeasureWidth(l, Font::Body));
    const float vx = kRowPadX + labelW + 24.0f;
    size_t line = 0;
    for (size_t i = 0; i < it.details.labels.size(); ++i) {
        if (it.details.hidden[i]) continue;
        const float top = it.y + static_cast<float>(line) * kDetailLineHeight;
        if (line++ > 0) c.Line(1.0f, top, it.w - 1.0f, top, p.divider, 1.0f);
        c.Text(it.details.labels[i], Rect{kRowPadX, top, kRowPadX + labelW + 8.0f, top + kDetailLineHeight}, Font::Body, p.text);
        c.Text(it.details.values[i], Rect{vx, top, it.w - kRowPadX, top + kDetailLineHeight}, Font::Body, p.textSecondary,
               HAlign::Right);
    }
}

void Page::Draw(Canvas& c, const Palette& p, const Gfx& g) {
    for (Item& it : items_) {
        switch (it.kind) {
        case Item::Kind::Section:
            if (p.promptHeaders) {
                std::wstring up = it.text;
                for (wchar_t& ch : up) ch = static_cast<wchar_t>(std::towupper(ch));
                c.Text(L"[ " + up + L" ]", Rect{0, it.y + 8.0f, it.w, it.y + kSectionHeight - 2.0f}, Font::Section, p.accent, HAlign::Left);
            } else {
                c.Text(it.text, Rect{0, it.y + 8.0f, it.w, it.y + kSectionHeight - 2.0f}, Font::Section, p.text, HAlign::Left);
            }
            break;
        case Item::Kind::Info:
            if (it.info->visible) it.info->Draw(c, p);
            break;
        case Item::Kind::Tiles:
            for (Tile* t : it.tiles) t->Draw(c, p);
            break;
        case Item::Kind::Details:
            DrawDetails(c, p, g, it);
            break;
        case Item::Kind::Free:
            if (it.free->visible) it.free->Draw(c, p);
            break;
        case Item::Kind::Card:
            DrawCard(c, p, g, it);
            break;
        }
    }
}

Control* Page::HitTest(float x, float y) const {
    for (auto it = focusOrder_.rbegin(); it != focusOrder_.rend(); ++it) {
        Control* c = *it;
        if (c->visible && c->enabled && c->interactive && RectHas(c->bounds, x, y)) return c;
    }
    return nullptr;
}

bool Page::Tick(float dt) {
    bool again = false;
    for (auto& c : controls_) again |= c->Tick(dt);
    return again;
}

}  // namespace moonsr::app::ui
