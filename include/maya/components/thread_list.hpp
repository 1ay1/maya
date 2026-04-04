#pragma once
// maya::components::ThreadList — Conversation history with time groups
//
//   struct Thread { std::string title, time_label; int id; };
//   ThreadList<Thread> threads({
//       .items = my_threads,
//       .groups = {
//           {.label = "Today", .start = 0, .count = 3},
//           {.label = "Yesterday", .start = 3, .count = 2},
//       },
//       .render_item = [](const Thread& t, bool selected) { ... }
//   });

#include "core.hpp"
#include "list.hpp"

namespace maya::components {

struct ThreadGroup {
    std::string label;
    int         start;   // index into items
    int         count;
};

struct ThreadEntry {
    std::string title;
    std::string time_label;
    std::string subtitle = "";
    int         id       = 0;
};

struct ThreadListProps {
    std::vector<ThreadEntry>  items  = {};
    std::vector<ThreadGroup>  groups = {};
    int                       max_visible = 15;
    std::string               empty_text  = "No conversations yet";
};

class ThreadList {
    List<int> list_;   // indices into items
    ThreadListProps props_;
    std::string filter_;

    std::vector<int> filtered_indices() const {
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(props_.items.size()); ++i) {
            if (filter_.empty()) {
                indices.push_back(i);
                continue;
            }
            // Simple case-insensitive substring match
            auto& title = props_.items[i].title;
            bool match = false;
            for (size_t j = 0; j + filter_.size() <= title.size(); ++j) {
                bool ok = true;
                for (size_t k = 0; k < filter_.size(); ++k) {
                    char a = title[j + k], b = filter_[k];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { ok = false; break; }
                }
                if (ok) { match = true; break; }
            }
            if (match) indices.push_back(i);
        }
        return indices;
    }

public:
    explicit ThreadList(ThreadListProps props = {})
        : list_(ListProps<int>{.max_visible = props.max_visible})
        , props_(std::move(props)) {
        refresh();
    }

    void refresh() {
        list_.set_items(filtered_indices());
    }

    void set_filter(const std::string& f) {
        filter_ = f;
        refresh();
    }

    [[nodiscard]] int selected_index() const {
        auto* item = list_.selected_item();
        return item ? *item : -1;
    }

    [[nodiscard]] const ThreadEntry* selected_entry() const {
        auto* item = list_.selected_item();
        if (!item || *item < 0 || *item >= static_cast<int>(props_.items.size()))
            return nullptr;
        return &props_.items[*item];
    }

    bool update(const Event& ev) {
        return list_.update(ev);
    }

    [[nodiscard]] Element render() const {
        using namespace maya::dsl;

        auto& p = palette();

        if (list_.empty()) {
            return text(props_.empty_text, Style{}.with_italic().with_fg(p.dim));
        }

        // Determine which group each visible item belongs to
        return list_.render([&](const int& idx, int /*list_i*/, bool selected) -> Element {
            auto& entry = props_.items[idx];

            // Find group label for this index
            std::string group_label;
            for (auto& g : props_.groups) {
                if (idx == g.start) {
                    group_label = g.label;
                    break;
                }
            }

            std::vector<Element> rows;

            // Group header (if this item starts a group)
            if (!group_label.empty()) {
                rows.push_back(text("  " + group_label,
                                     Style{}.with_bold().with_fg(p.muted)));
            }

            // Thread entry
            Style title_style = selected
                ? Style{}.with_bold().with_fg(p.primary)
                : Style{}.with_fg(p.text);

            std::vector<Element> row;
            row.push_back(text(selected ? "▸ " : "  ", Style{}.with_fg(p.primary)));
            row.push_back(text(entry.title, title_style));

            if (!entry.time_label.empty()) {
                row.push_back(Element(space));
                row.push_back(text(entry.time_label, Style{}.with_fg(p.dim)));
            }

            rows.push_back(hstack()(std::move(row)));

            if (!entry.subtitle.empty()) {
                rows.push_back(text("    " + entry.subtitle,
                                     Style{}.with_fg(p.muted)));
            }

            if (rows.size() == 1) return std::move(rows[0]);
            return vstack()(std::move(rows));
        });
    }
};

} // namespace maya::components
