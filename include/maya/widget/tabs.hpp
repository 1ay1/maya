#pragma once
// maya::widget::tabs — Tabbed content switcher
//
// Arrow-key navigable tab bar with focus integration.
// Supports Left/Right, h/l, and 1-9 number keys.
//
// Usage:
//   Tabs tabs({"General", "Editor", "Keybindings"});
//   tabs.on_change([](int idx) { /* switch content */ });
//   auto ui = v(tabs, content);

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

class Tabs {
    std::vector<std::string> labels_;
    Signal<int> active_{0};
    FocusNode focus_;

    std::move_only_function<void(int)> on_change_;

public:
    explicit Tabs(std::vector<std::string> labels)
        : labels_(std::move(labels)) {}

    Tabs(std::initializer_list<std::string_view> labels) {
        labels_.reserve(labels.size());
        for (auto sv : labels) labels_.emplace_back(sv);
    }

    // -- Accessors --
    [[nodiscard]] const Signal<int>& active()    const { return active_; }
    [[nodiscard]] FocusNode& focus_node()               { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()  const { return focus_; }
    [[nodiscard]] int size()                     const { return static_cast<int>(labels_.size()); }

    // -- Callback --
    template <std::invocable<int> F>
    void on_change(F&& fn) { on_change_ = std::forward<F>(fn); }

    // -- Mutation --
    void set_active(int idx) {
        if (idx >= 0 && idx < static_cast<int>(labels_.size())) {
            active_.set(idx);
            if (on_change_) on_change_(idx);
        }
    }

    void add_tab(std::string label) {
        labels_.push_back(std::move(label));
    }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;
        if (labels_.empty()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Left:  move_prev(); return true;
                    case SpecialKey::Right: move_next(); return true;
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'h') { move_prev(); return true; }
                if (ck.codepoint == 'l') { move_next(); return true; }
                // 1-9 number keys for direct tab selection
                if (ck.codepoint >= '1' && ck.codepoint <= '9') {
                    int idx = static_cast<int>(ck.codepoint - '1');
                    if (idx < static_cast<int>(labels_.size())) {
                        active_.set(idx);
                        if (on_change_) on_change_(idx);
                    }
                    return true;
                }
                return false;
            },
        }, ev.key);
    }

    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int cur = active_();

        auto active_style = Style{}.with_bold().with_underline()
                                   .with_fg(Color::rgb(97, 175, 239));
        auto inactive_style = Style{}.with_fg(Color::rgb(150, 156, 170));
        auto sep_style = Style{}.with_fg(Color::rgb(92, 99, 112));

        // Build tab labels as a single TextElement with StyledRuns
        std::string content;
        std::vector<StyledRun> runs;

        for (int i = 0; i < static_cast<int>(labels_.size()); ++i) {
            if (i > 0) {
                std::string sep = " \xe2\x94\x82 "; // " │ "
                runs.push_back(StyledRun{content.size(), sep.size(), sep_style});
                content += sep;
            }
            const auto& label = labels_[static_cast<size_t>(i)];
            runs.push_back(StyledRun{
                content.size(), label.size(),
                (i == cur) ? active_style : inactive_style,
            });
            content += label;
        }

        auto tab_row = Element{TextElement{
            .content = std::move(content),
            .style = inactive_style,
            .runs = std::move(runs),
        }};

        // Full-width separator line below tabs
        auto separator = Element{ComponentElement{
            .render = [](int w, int /*h*/) -> Element {
                std::string line;
                for (int i = 0; i < w; ++i) line += "\xe2\x94\x80"; // "─"
                return Element{TextElement{
                    .content = std::move(line),
                    .style = Style{}.with_fg(Color::rgb(50, 54, 62)),
                }};
            },
            .layout = {},
        }};

        return dsl::v(std::move(tab_row), std::move(separator)).build();
    }

private:
    void move_prev() {
        int c = active_();
        int n = static_cast<int>(labels_.size());
        int next = (c > 0) ? c - 1 : n - 1;
        active_.set(next);
        if (on_change_) on_change_(next);
    }

    void move_next() {
        int c = active_();
        int n = static_cast<int>(labels_.size());
        int next = (c + 1) % n;
        active_.set(next);
        if (on_change_) on_change_(next);
    }
};

} // namespace maya
