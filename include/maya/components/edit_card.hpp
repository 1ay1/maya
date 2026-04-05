#pragma once
// maya::components::EditCard — File edit card with inline diff + accept/reject per hunk
//
//   EditCard card({.file_path = "src/main.cpp", .diff = unified_diff});
//   card.update(ev);
//   auto ui = card.render(frame);

#include "core.hpp"
#include "diff_view.hpp"
#include "key_binding.hpp"

namespace maya::components {

enum class EditDecision { Pending, Accepted, Rejected };

struct Hunk {
    std::string   header;
    std::string   content;
    EditDecision  decision = EditDecision::Pending;
};

struct EditCardProps {
    std::string file_path;
    std::string diff;
    TaskStatus  status       = TaskStatus::Pending;
    int         added        = -1;   // -1 = auto-count from diff
    int         removed      = -1;
    bool        collapsed    = false;
    bool        show_keyhints = true;
};

class EditCard {
    std::vector<Hunk> hunks_;
    bool collapsed_;
    int  selected_ = 0;
    EditCardProps props_;

    void parse_hunks();
    void count_stats();

public:
    explicit EditCard(EditCardProps props = {})
        : collapsed_(props.collapsed), props_(std::move(props))
    {
        parse_hunks();
        count_stats();
    }

    [[nodiscard]] bool accepted() const;
    [[nodiscard]] bool rejected() const;
    [[nodiscard]] EditDecision decision() const;

    void toggle_collapsed() { collapsed_ = !collapsed_; }
    void accept_all() { for (auto& h : hunks_) h.decision = EditDecision::Accepted; }
    void reject_all() { for (auto& h : hunks_) h.decision = EditDecision::Rejected; }

    void accept_hunk(int i);
    void reject_hunk(int i);
    void update(const Event& ev);

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
