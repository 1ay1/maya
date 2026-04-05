#pragma once
// maya::components::CommandCard — Bash/terminal command with output
//
//   CommandCard card({.command = "npm test", .status = TaskStatus::InProgress});
//   card.set_output(output_text);
//   card.set_exit_code(0);
//   card.update(ev);
//   auto ui = card.render(frame);

#include "core.hpp"
#include "key_binding.hpp"

namespace maya::components {

struct CommandCardProps {
    std::string command;
    std::string output      = "";
    int         exit_code   = -1;     // -1 = not finished
    TaskStatus  status      = TaskStatus::Pending;
    int         frame       = 0;
    bool        collapsed   = false;
    bool        dangerous   = false;
    int         max_lines   = 20;
    Element     permission  = {};     // permission prompt element
};

class CommandCard {
    bool collapsed_;
    CommandCardProps props_;

public:
    explicit CommandCard(CommandCardProps props = {})
        : collapsed_(props.collapsed), props_(std::move(props)) {}

    void set_output(std::string out) { props_.output = std::move(out); }
    void set_exit_code(int code) { props_.exit_code = code; }
    void set_status(TaskStatus st) { props_.status = st; }
    void toggle_collapsed() { collapsed_ = !collapsed_; }

    void update(const Event& ev);

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
