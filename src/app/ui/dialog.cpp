#include "dialog.h"

#include <algorithm>

namespace moonsr::app::ui {

namespace {
constexpr float kPad = 24.0f;
constexpr float kLabelW = 118.0f;
constexpr float kCopyW = 52.0f;
constexpr float kBtnH = 32.0f;
}  // namespace

ModalDialog::ModalDialog(Spec spec) : spec_(std::move(spec)) {
    auto prim = std::make_unique<Button>();
    prim->accent = true;
    prim->text = spec_.primary;
    items_.push_back(Item{Item::Kind::Primary, -1, std::move(prim)});
    if (spec_.showSecondary && !spec_.secondary.empty()) {
        auto sec = std::make_unique<Button>();
        sec->text = spec_.secondary;
        items_.push_back(Item{Item::Kind::Secondary, -1, std::move(sec)});
    }
    for (size_t i = 0; i < spec_.lines.size(); ++i) {
        if (spec_.lines[i].copyText.empty()) continue;
        auto link = std::make_unique<LinkButton>();
        link->text = L"Copy";
        items_.push_back(Item{Item::Kind::Copy, static_cast<int>(i), std::move(link)});
    }
    for (Item& it : items_) it.ctl->enabled = true;
}

void ModalDialog::Layout(const Gfx& g, float w, float h) {
    const float pw = std::min(540.0f, w - 32.0f);
    valueX_ = kPad + kLabelW + 8.0f;  // relative to the panel
    lineY_.clear();
    lineH_.clear();
    float y = kPad + 32.0f + 8.0f;
    for (const Line& l : spec_.lines) {
        const float vw = pw - kPad - valueX_ - (l.copyText.empty() ? 0.0f : kCopyW + 8.0f);
        const float th = g.MeasureHeight(l.value, Font::Body, std::max(40.0f, vw));
        const float lh = std::max(28.0f, th + 8.0f);
        lineY_.push_back(y);
        lineH_.push_back(lh);
        y += lh;
    }
    valueW_ = pw - kPad - valueX_;
    if (!spec_.body.empty()) {
        y += 10.0f;
        bodyY_ = y;
        bodyH_ = g.MeasureHeight(spec_.body, Font::Body, pw - 2.0f * kPad);
        y += bodyH_;
    } else {
        bodyH_ = 0.0f;
    }
    y += kPad;
    const float buttonsY = y;
    y += kBtnH + kPad;
    const float ph = y;
    const float px = (w - pw) * 0.5f;
    const float py = std::max(12.0f, (h - ph) * 0.5f);
    panel_ = Rect{px, py, px + pw, py + ph};

    // Buttons: primary on the left of the pair, both right aligned.
    float right = panel_.right - kPad;
    std::vector<Item*> btns;
    for (Item& it : items_)
        if (it.kind != Item::Kind::Copy) btns.push_back(&it);
    for (size_t i = btns.size(); i-- > 0;) {
        // Secondary is rightmost, primary to its left.
        Item* it = btns[i];
        const float bw = std::max(110.0f, it->ctl->PreferredWidth(g));
        it->ctl->bounds = Rect{right - bw, panel_.top + buttonsY, right, panel_.top + buttonsY + kBtnH};
        right -= bw + 8.0f;
    }
    for (Item& it : items_) {
        if (it.kind != Item::Kind::Copy) continue;
        const float ly = panel_.top + lineY_[static_cast<size_t>(it.line)];
        it.ctl->bounds = Rect{panel_.right - kPad - kCopyW, ly + 2.0f, panel_.right - kPad, ly + 26.0f};
    }
}

void ModalDialog::Draw(Canvas& c, const Palette& p, float w, float h) {
    const Gfx& g = c.gfx();
    Layout(g, w, h);
    c.FillRect(Rect{0, 0, w, h}, p.dark ? D2D1_COLOR_F{0, 0, 0, 0.55f} : D2D1_COLOR_F{0, 0, 0, 0.32f});
    if (!p.retro)
      for (int i = 6; i >= 1; --i)
        c.FillRound(Inflate(panel_, static_cast<float>(i) * 2.0f), 8.0f + static_cast<float>(i) * 2.0f,
                    D2D1_COLOR_F{0, 0, 0, 0.035f});
    c.FillRound(panel_, 8.0f, p.popupFill);
    c.StrokeRound(panel_, 8.0f, p.popupStroke);

    auto tone = [&](Tone t) {
        switch (t) {
        case Tone::Secondary: return p.textSecondary;
        case Tone::Error: return p.dark ? p.error.icon : p.error.icon;
        case Tone::Warning: return p.dark ? p.warning.icon : p.warning.icon;
        default: return p.text;
        }
    };

    c.Text(spec_.title, Rect{panel_.left + kPad, panel_.top + kPad, panel_.right - kPad, panel_.top + kPad + 32.0f}, Font::Section,
           p.text);
    for (size_t i = 0; i < spec_.lines.size(); ++i) {
        const Line& l = spec_.lines[i];
        const float top = panel_.top + lineY_[i];
        const float lh = lineH_[i];
        c.Text(l.label, Rect{panel_.left + kPad, top, panel_.left + kPad + kLabelW, top + 28.0f}, Font::Caption, p.textSecondary);
        const float vr = panel_.right - kPad - (l.copyText.empty() ? 0.0f : kCopyW + 8.0f);
        c.Text(l.value, Rect{panel_.left + valueX_, top + 4.0f, vr, top + lh}, Font::Body, tone(l.tone), HAlign::Left, true);
    }
    if (!spec_.body.empty())
        c.Text(spec_.body, Rect{panel_.left + kPad, panel_.top + bodyY_, panel_.right - kPad, panel_.top + bodyY_ + bodyH_ + 4.0f},
               Font::Body, tone(spec_.bodyTone), HAlign::Left, true);

    for (size_t i = 0; i < items_.size(); ++i) {
        Item& it = items_[i];
        it.ctl->focused = static_cast<int>(i) == focus_;
        it.ctl->focusVisible = focusVisible_ && it.ctl->focused;
        it.ctl->pressed = static_cast<int>(i) == pressed_;
        if (it.kind == Item::Kind::Copy) {
            auto* link = static_cast<LinkButton*>(it.ctl.get());
            link->text = copiedLine_ == it.line ? L"Copied" : L"Copy";
        }
        it.ctl->Draw(c, p);
    }
}

int ModalDialog::ItemAt(float x, float y) const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (RectHas(items_[i].ctl->bounds, x, y)) return static_cast<int>(i);
    return -1;
}

bool ModalDialog::WantsHand() const {
    for (const Item& it : items_)
        if (it.ctl->hover) return true;
    return false;
}

void ModalDialog::MouseMove(float x, float y) {
    const int at = ItemAt(x, y);
    for (size_t i = 0; i < items_.size(); ++i) items_[i].ctl->hover = static_cast<int>(i) == at;
}

void ModalDialog::MouseDown(float x, float y) {
    pressed_ = ItemAt(x, y);
    if (pressed_ >= 0) {
        focus_ = pressed_;
        focusVisible_ = false;
    }
}

void ModalDialog::MouseUp(float x, float y) {
    const int at = ItemAt(x, y);
    const int was = pressed_;
    pressed_ = -1;
    if (was >= 0 && was == at) Activate(at);
}

void ModalDialog::Activate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];
    switch (it.kind) {
    case Item::Kind::Primary: result_ = Result::Primary; break;
    case Item::Kind::Secondary: result_ = Result::Secondary; break;
    case Item::Kind::Copy:
        if (spec_.onCopy) spec_.onCopy(spec_.lines[static_cast<size_t>(it.line)].copyText);
        copiedLine_ = it.line;
        break;
    }
}

bool ModalDialog::Key(UINT vk, bool shift) {
    const int n = static_cast<int>(items_.size());
    if (vk == VK_ESCAPE) {
        result_ = items_.size() > 1 && items_[1].kind == Item::Kind::Secondary ? Result::Secondary : Result::Primary;
    } else if (vk == VK_RETURN) {
        Activate(focusVisible_ && focus_ >= 0 ? focus_ : 0);
    } else if (vk == VK_SPACE) {
        Activate(focus_);
    } else if (vk == VK_TAB && n > 0) {
        focusVisible_ = true;
        focus_ = ((focus_ + (shift ? -1 : 1)) % n + n) % n;
    }
    return true;
}

}  // namespace moonsr::app::ui
