#pragma once
// maya::components::Checkpoint — Rollback point with restore button
//
//   Checkpoint cp({.label = "Before refactor",
//                  .file_count = 3, .added = 42, .removed = 15,
//                  .timestamp = "2m ago"});
//   cp.update(ev);
//   auto ui = cp.render();

#include "core.hpp"
#include "key_binding.hpp"

namespace maya::components {

struct CheckpointProps {
    std::string label;
    int         file_count    = 0;
    int         added         = 0;
    int         removed       = 0;
    std::string timestamp     = "";
    bool        show_keyhints = true;
};

class Checkpoint {
    bool restored_  = false;
    bool confirmed_ = false;
    CheckpointProps props_;

public:
    explicit Checkpoint(CheckpointProps props = {})
        : props_(std::move(props)) {}

    [[nodiscard]] bool restored() const { return restored_; }
    [[nodiscard]] bool confirming() const { return confirmed_; }

    void update(const Event& ev);

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
