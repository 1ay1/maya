#pragma once
// maya::widget::conversation_view — Scrollable conversation container
//
// The spine of a Claude Code agent UI. Manages an ordered list of
// conversation items (user messages, assistant text, tool calls,
// thinking blocks) with auto-scroll and turn grouping.
//
//   ConversationView conv;
//   conv.push(UserMessage::build("fix the bug"));
//   conv.push(thinking_block);
//   conv.push(tool_card);
//   conv.push(AssistantMessage::build(markdown));
//   auto ui = conv.build();

#include <cstddef>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class ConversationView {
    std::vector<Element> items_;
    int  max_visible_ = 0;   // 0 = show all (no scrolling limit)
    bool auto_scroll_ = true;
    int  scroll_pos_  = 0;   // offset from bottom when not auto-scrolling
    int  gap_         = 1;   // vertical gap between items

public:
    ConversationView() = default;

    void push(Element item) { items_.push_back(std::move(item)); }
    void clear() { items_.clear(); scroll_pos_ = 0; }

    void set_max_visible(int n) { max_visible_ = n; }
    void set_auto_scroll(bool b) { auto_scroll_ = b; }
    void set_gap(int g) { gap_ = g; }

    void scroll_up(int n = 1) {
        auto_scroll_ = false;
        scroll_pos_ = std::min(scroll_pos_ + n,
                               std::max(0, static_cast<int>(items_.size()) - 1));
    }
    void scroll_down(int n = 1) {
        scroll_pos_ = std::max(0, scroll_pos_ - n);
        if (scroll_pos_ == 0) auto_scroll_ = true;
    }
    void scroll_to_bottom() {
        scroll_pos_ = 0;
        auto_scroll_ = true;
    }

    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (items_.empty()) return text("");

        // Determine visible range
        int total = static_cast<int>(items_.size());
        int visible = (max_visible_ > 0) ? std::min(max_visible_, total) : total;

        int end_idx = total - scroll_pos_;
        int start_idx = std::max(0, end_idx - visible);
        end_idx = std::min(end_idx, total);

        std::vector<Element> visible_items;
        visible_items.reserve(static_cast<size_t>(end_idx - start_idx));

        for (int i = start_idx; i < end_idx; ++i)
            visible_items.push_back(items_[static_cast<size_t>(i)]);

        // Scroll indicator if not showing everything
        if (start_idx > 0) {
            auto indicator = std::to_string(start_idx) + " more above";
            visible_items.insert(visible_items.begin(),
                text(std::move(indicator),
                     Style{}.with_fg(Color::rgb(92, 99, 112)).with_italic()));
        }

        return (v(std::move(visible_items)) | gap(gap_)).build();
    }
};

} // namespace maya
