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

    std::vector<int> filtered_indices() const;

public:
    explicit ThreadList(ThreadListProps props = {})
        : list_(ListProps<int>{.max_visible = props.max_visible})
        , props_(std::move(props)) {
        refresh();
    }

    void refresh();
    void set_filter(const std::string& f);
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] const ThreadEntry* selected_entry() const;
    bool update(const Event& ev);
    [[nodiscard]] Element render() const;
};

} // namespace maya::components
