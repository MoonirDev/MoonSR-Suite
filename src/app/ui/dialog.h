// A modal Fluent-style dialog drawn over the whole window by View: dimmed backdrop, rounded panel, title, optional
// label / value lines (a line can offer a "Copy" link), a body paragraph and a primary + secondary button.
// Esc = secondary (cancel), Enter = primary (or the focused item after Tab), Tab / Shift+Tab cycles the items.
// View owns the input routing and calls the callbacks of the Spec after the dialog has been closed.
// Threading: UI thread only.
#pragma once

#include "controls.h"
#include "gfx.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moonsr::app::ui {

class ModalDialog {
public:
    enum class Tone { Normal, Secondary, Error, Warning };

    struct Line {
        std::wstring label;
        std::wstring value;
        Tone tone = Tone::Normal;
        std::wstring copyText;  // non-empty: the line gets a "Copy" link that passes this text to Spec::onCopy
    };

    struct Spec {
        std::wstring title;
        std::vector<Line> lines;
        std::wstring body;       // paragraph under the lines
        Tone bodyTone = Tone::Normal;
        std::wstring primary = L"OK";
        std::wstring secondary;  // empty: no secondary button (Esc then closes with the primary's "cancel" meaning)
        bool showSecondary = true;
        std::function<void()> onPrimary;
        std::function<void()> onSecondary;
        std::function<void(const std::wstring&)> onCopy;
    };

    explicit ModalDialog(Spec spec);

    // `w`, `h`: client size in DIPs.
    void Draw(Canvas& c, const Palette& p, float w, float h);
    void MouseMove(float x, float y);
    void MouseDown(float x, float y);
    void MouseUp(float x, float y);
    bool Key(UINT vk, bool shift);  // always consumes
    bool WantsHand() const;

    enum class Result { None, Primary, Secondary };
    Result result() const { return result_; }
    Spec TakeSpec() { return std::move(spec_); }

private:
    struct Item {
        enum class Kind { Primary, Secondary, Copy } kind = Kind::Primary;
        int line = -1;
        std::unique_ptr<Control> ctl;  // a Button or LinkButton; its bounds / hover / pressed flags are kept current
    };

    void Layout(const Gfx& g, float w, float h);
    int ItemAt(float x, float y) const;
    void Activate(int index);

    Spec spec_;
    std::vector<Item> items_;
    std::vector<float> lineY_, lineH_;
    Rect panel_{};
    float bodyY_ = 0, bodyH_ = 0, valueX_ = 0, valueW_ = 0;
    int focus_ = 0;
    bool focusVisible_ = false;
    int pressed_ = -1;
    int copiedLine_ = -1;
    Result result_ = Result::None;
};

}  // namespace moonsr::app::ui
