#pragma once
// maya::components::ModelSelector — AI model/provider picker
//
//   ModelSelector selector({.models = {
//       {.name = "claude-opus-4-6", .provider = "Anthropic", .color = Color::rgb(200, 120, 255)},
//       {.name = "claude-sonnet-4-6", .provider = "Anthropic", .color = Color::rgb(100, 160, 255)},
//   }});
//   selector.update(ev);
//   auto ui = selector.render();

#include "core.hpp"

namespace maya::components {

struct ModelInfo {
    std::string name;
    std::string provider;
    std::string icon        = "\xe2\x97\x86";  // ◆
    std::string context     = "";
    std::string cost_label  = "";
    Color       color       = palette().primary;
};

struct ModelSelectorProps {
    std::vector<ModelInfo> models = {};
    int         selected = 0;
    std::string label    = "";
};

class ModelSelector {
    int  selected_ = 0;
    bool open_     = false;
    int  hover_    = 0;
    ModelSelectorProps props_;

public:
    explicit ModelSelector(ModelSelectorProps props = {})
        : selected_(props.selected), hover_(props.selected), props_(std::move(props)) {}

    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] bool is_open() const { return open_; }
    void set_selected(int i) { selected_ = i; }

    [[nodiscard]] const ModelInfo* selected_model() const;

    bool update(const Event& ev);
};

} // namespace maya::components
