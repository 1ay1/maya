#pragma once
// maya::components::SearchInput — Single-line search with live filtering
//
//   SearchInput search({.placeholder = "Search threads..."});
//
//   // In event handler:
//   search.update(ev);
//
//   // Access query:
//   search.query()
//
//   // Render:
//   search.render()

#include "core.hpp"

namespace maya::components {

struct SearchInputProps {
    std::string placeholder = "Search...";
    std::string icon        = "🔍";
    Color       color       = palette().primary;
    bool        focused     = true;
};

class SearchInput {
    std::string buf_;
    int cursor_ = 0;
    bool focused_;
    SearchInputProps props_;

    bool is_word_char(char c) const;
    int word_left(int pos) const;
    int word_right(int pos) const;

public:
    explicit SearchInput(SearchInputProps props = {})
        : focused_(props.focused), props_(std::move(props)) {}

    [[nodiscard]] const std::string& query() const { return buf_; }
    void set_query(std::string q) { buf_ = std::move(q); cursor_ = static_cast<int>(buf_.size()); }
    void clear() { buf_.clear(); cursor_ = 0; }
    [[nodiscard]] bool empty() const { return buf_.empty(); }
    void focus() { focused_ = true; }
    void blur() { focused_ = false; }

    bool update(const Event& ev);
    [[nodiscard]] Element render() const;
};

} // namespace maya::components
