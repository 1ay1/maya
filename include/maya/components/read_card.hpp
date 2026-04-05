#pragma once
// maya::components::ReadCard — Collapsible file read with line count
//
//   ReadCard card({.file_path = "src/main.cpp", .content = file_text});
//   card.update(ev);
//   auto ui = card.render(frame);

#include "core.hpp"
#include "code_block.hpp"

namespace maya::components {

struct ReadCardProps {
    std::string file_path;
    std::string content     = "";
    int         start_line  = 1;
    int         end_line    = 0;      // 0 = auto
    int         line_count  = 0;      // 0 = auto
    TaskStatus  status      = TaskStatus::Pending;
    bool        collapsed   = true;
    int         max_lines   = 30;
    std::string language    = "";
};

class ReadCard {
    bool collapsed_;
    ReadCardProps props_;

    [[nodiscard]] int auto_count() const;


public:
    explicit ReadCard(ReadCardProps props = {})
        : collapsed_(props.collapsed), props_(std::move(props)) {}

    void set_content(std::string c) { props_.content = std::move(c); }
    void set_status(TaskStatus st) { props_.status = st; }
    void toggle_collapsed() { collapsed_ = !collapsed_; }
    void set_collapsed(bool v) { collapsed_ = v; }

    void update(const Event& ev);

    [[nodiscard]] Element render(int frame = 0) const;
};

} // namespace maya::components
