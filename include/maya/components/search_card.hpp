#pragma once
// maya::components::SearchCard — Collapsible search results card
//
//   SearchCard card({.query = "handleClick",
//                    .results = {{.file_path = "src/app.tsx", .match_count = 3}},
//                    .status = TaskStatus::Completed});
//   card.update(ev);
//   auto ui = card.render(frame);

#include "core.hpp"

namespace maya::components {

struct SearchResult {
    std::string file_path;
    int         match_count = 0;
    std::string preview     = "";
};

struct SearchCardProps {
    std::string query;
    std::vector<SearchResult> results = {};
    TaskStatus  status      = TaskStatus::Pending;
    bool        collapsed   = true;
    std::string tool_name   = "search";
    int         max_results = 20;
};

class SearchCard {
    bool collapsed_;
    SearchCardProps props_;

public:
    explicit SearchCard(SearchCardProps props = {})
        : collapsed_(props.collapsed), props_(std::move(props)) {}

    void set_results(std::vector<SearchResult> r) { props_.results = std::move(r); }
    void set_status(TaskStatus st) { props_.status = st; }
    void toggle_collapsed() { collapsed_ = !collapsed_; }

    void update(const Event& ev);

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
