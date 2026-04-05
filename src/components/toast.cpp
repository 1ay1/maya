#include "maya/components/toast.hpp"

#include <algorithm>

namespace maya::components {

void ToastStack::push(std::string text, Severity severity, float ttl) {
    messages_.push_back(ToastMessage{
        .text     = std::move(text),
        .severity = severity,
        .ttl      = ttl,
        .elapsed  = 0.f,
    });
}

void ToastStack::tick(float dt) {
    for (auto& msg : messages_) {
        msg.elapsed += dt;
    }
    std::erase_if(messages_, [](const ToastMessage& m) {
        return m.elapsed >= m.ttl;
    });
}

Element ToastStack::render() const {
    using namespace maya::dsl;

    if (messages_.empty()) {
        return text("");
    }

    std::vector<Element> rows;

    // Show at most max_visible, newest at bottom
    int start = static_cast<int>(messages_.size()) - max_visible_;
    if (start < 0) start = 0;

    for (int i = start; i < static_cast<int>(messages_.size()); ++i) {
        const auto& msg = messages_[static_cast<std::size_t>(i)];
        Color c = severity_color(msg.severity);
        const char* icon = severity_icon(msg.severity);

        std::vector<Element> line;
        line.push_back(text(icon, Style{}.with_bold().with_fg(c)));
        line.push_back(text(msg.text, Style{}.with_fg(palette().text)));

        if (show_timer_) {
            float remaining = msg.ttl - msg.elapsed;
            if (remaining < 0.f) remaining = 0.f;
            line.push_back(
                text(fmt("(%.1fs)", remaining),
                     Style{}.with_fg(palette().muted))
            );
        }

        rows.push_back(
            hstack().gap(1)
                .border(BorderStyle::Round)
                .border_color(c)
                .padding(0, 1, 0, 1)(std::move(line))
        );
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
