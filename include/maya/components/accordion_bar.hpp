#pragma once
// maya::components::AccordionBar — File change summary bar (Zed style)
//
//   AccordionBar bar({.files = {{.path = "src/main.cpp", .added = 12, .removed = 3}}});
//   bar.update(ev);
//   auto ui = bar.render();

#include "core.hpp"
#include "diff_stat.hpp"

namespace maya::components {

struct ChangedFile {
    std::string path;
    int         added      = 0;
    int         removed    = 0;
    bool        is_new     = false;
    bool        is_deleted = false;
};

struct AccordionBarProps {
    std::vector<ChangedFile> files = {};
    bool show_keyhints = true;
};

class AccordionBar {
    bool expanded_ = false;
    AccordionBarProps props_;

public:
    explicit AccordionBar(AccordionBarProps props = {})
        : props_(std::move(props)) {}

    void set_files(std::vector<ChangedFile> f) { props_.files = std::move(f); }

    void add_file(ChangedFile f) { props_.files.push_back(std::move(f)); }
    void clear() { props_.files.clear(); }
    void toggle() { expanded_ = !expanded_; }

    [[nodiscard]] bool expanded() const { return expanded_; }
    [[nodiscard]] bool empty() const { return props_.files.empty(); }
    [[nodiscard]] int file_count() const { return static_cast<int>(props_.files.size()); }

    void update(const Event& ev);

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
