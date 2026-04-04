#pragma once
// maya::components::ScrollView — Virtual scrolling container
//
//   ScrollView scroll({.max_visible = 20});
//
//   // In event handler:
//   scroll.update(ev);
//
//   // In render — pass total items and a render function:
//   scroll.render(total_items, [&](int i) { return render_row(i); })
//
// Supports: scroll wheel, PgUp/PgDn, Ctrl+Home/End, tail-follow mode.

#include "core.hpp"

namespace maya::components {

struct ScrollViewProps {
    int  max_visible  = 20;
    bool tail_follow  = false;   // auto-scroll to bottom on new content
    bool show_scrollbar = true;
};

class ScrollView {
    int offset_    = 0;
    int total_     = 0;
    bool following_ = false;
    ScrollViewProps props_;

    void clamp() {
        int max_off = std::max(0, total_ - props_.max_visible);
        offset_ = std::clamp(offset_, 0, max_off);
    }

public:
    explicit ScrollView(ScrollViewProps props = {})
        : following_(props.tail_follow), props_(std::move(props)) {}

    [[nodiscard]] int offset() const { return offset_; }
    [[nodiscard]] bool at_bottom() const {
        return offset_ >= total_ - props_.max_visible;
    }

    void scroll_to(int pos) { offset_ = pos; clamp(); }
    void scroll_to_top() { offset_ = 0; }
    void scroll_to_bottom() { offset_ = std::max(0, total_ - props_.max_visible); }

    void set_total(int n) {
        total_ = n;
        if (following_ && at_bottom()) scroll_to_bottom();
        clamp();
    }

    bool update(const Event& ev) {
        if (scrolled_up(ev) || key(ev, SpecialKey::Up)) {
            offset_--; following_ = false; clamp(); return true;
        }
        if (scrolled_down(ev) || key(ev, SpecialKey::Down)) {
            offset_++; clamp();
            if (at_bottom()) following_ = true;
            return true;
        }
        if (key(ev, SpecialKey::PageUp)) {
            offset_ -= props_.max_visible; following_ = false; clamp(); return true;
        }
        if (key(ev, SpecialKey::PageDown)) {
            offset_ += props_.max_visible; clamp();
            if (at_bottom()) following_ = true;
            return true;
        }
        if (ctrl(ev, 'a')) { // Home
            scroll_to_top(); following_ = false; return true;
        }
        if (ctrl(ev, 'e')) { // End
            scroll_to_bottom(); following_ = true; return true;
        }

        return false;
    }

    // Render with a function that produces elements by index
    template <typename RenderFn>
    [[nodiscard]] Element render(int total, RenderFn&& render_item) {
        using namespace maya::dsl;

        total_ = total;
        if (following_) scroll_to_bottom();
        clamp();

        int vis_end = std::min(total_, offset_ + props_.max_visible);

        std::vector<Element> rows;
        rows.reserve(vis_end - offset_);

        for (int i = offset_; i < vis_end; ++i) {
            rows.push_back(render_item(i));
        }

        if (props_.show_scrollbar && total_ > props_.max_visible) {
            // Build simple text scrollbar indicator
            float ratio = static_cast<float>(offset_) /
                          static_cast<float>(std::max(1, total_ - props_.max_visible));
            int bar_h = std::max(1, props_.max_visible * props_.max_visible / std::max(1, total_));
            int bar_pos = static_cast<int>(ratio * (props_.max_visible - bar_h));

            std::vector<Element> with_scrollbar;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                bool in_thumb = (i >= bar_pos && i < bar_pos + bar_h);
                auto indicator = text(in_thumb ? "┃" : "│",
                                      Style{}.with_fg(in_thumb ? palette().primary : palette().dim));
                with_scrollbar.push_back(
                    hstack()(std::move(rows[i]),
                             Element(space),
                             std::move(indicator))
                );
            }
            return vstack()(std::move(with_scrollbar));
        }

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
