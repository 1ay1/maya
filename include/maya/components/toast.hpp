#pragma once
// maya::components::ToastStack — Notification/toast system with auto-dismiss
//
//   ToastStack toasts({.max_visible = 5, .show_timer = true});
//   toasts.push("File saved successfully", Severity::Success);
//   toasts.push("2 tests skipped", Severity::Warning, 5.0f);
//   toasts.tick(dt);
//   auto ui = toasts.render();

#include "core.hpp"

namespace maya::components {

struct ToastMessage {
    std::string text;
    Severity    severity  = Severity::Info;
    float       ttl       = 3.0f;     // time to live in seconds
    float       elapsed   = 0.f;      // internal timer
};

struct ToastStackProps {
    int  max_visible = 5;
    bool show_timer  = true;    // show remaining time
};

class ToastStack {
    std::vector<ToastMessage> messages_;
    int  max_visible_;
    bool show_timer_;

public:
    explicit ToastStack(ToastStackProps props = {})
        : max_visible_(props.max_visible)
        , show_timer_(props.show_timer) {}

    void push(std::string text, Severity severity = Severity::Info, float ttl = 3.0f);

    void tick(float dt);

    void clear() { messages_.clear(); }
    bool empty() const { return messages_.empty(); }
    int  count() const { return static_cast<int>(messages_.size()); }

    Element render() const;
};

} // namespace maya::components
