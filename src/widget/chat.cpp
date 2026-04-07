#include "maya/widget/chat.hpp"

#include "maya/element/builder.hpp"
#include "maya/style/style.hpp"

#include <string>
#include <vector>

namespace maya {

// ============================================================================
// Internal rendering helpers
// ============================================================================

Element ChatView::render_user_msg(const ChatMessage& msg) const {
    return detail::hstack()(
        Element{TextElement{
            .content = cfg_.prompt_icon,
            .style = cfg_.prompt_style,
        }},
        Element{TextElement{
            .content = msg.content,
            .style = cfg_.user_style,
        }}
    );
}

Element ChatView::render_tool_msg(const ChatMessage& msg) const {
    // Allow user override
    if (render_tool_fn_) return render_tool_fn_(msg);

    auto badge = tool_badge(msg.tool_name);

    // Adapt table column widths to terminal width
    int avail = std::max(available_width() - 4, 20);  // leave margin
    int prop_w = std::min(15, avail / 3);
    int val_w  = std::max(10, avail - prop_w - 4);  // 4 for padding/borders

    Table tbl({{"Property", prop_w}, {"Value", val_w}},
              {.header_style = cfg_.tool_header_style,
               .border_style = cfg_.tool_border_style});

    size_t pos = 0;
    while (pos < msg.content.size()) {
        size_t nl = msg.content.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? msg.content.substr(pos)
            : msg.content.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? msg.content.size() : nl + 1;

        auto colon = line.find(": ");
        if (colon != std::string::npos) {
            tbl.add_row({line.substr(0, colon), line.substr(colon + 2)});
        }
    }

    return detail::vstack()(
        detail::hstack()(
            badge,
            Element{TextElement{
                .content = " " + msg.tool_name,
                .style = Style{}.with_dim(),
            }}
        ),
        tbl
    );
}

Element ChatView::render_assistant_msg(const ChatMessage& msg) const {
    std::vector<Element> parts;
    // Compact label in narrow terminals
    std::string label = compact() ? "\xe2\x97\x86 AI" : cfg_.assistant_label;
    parts.push_back(Element{TextElement{
        .content = label,
        .style = cfg_.assistant_label_style,
    }});

    if (!msg.content.empty()) {
        if (msg.streaming_) {
            // Progressive markdown: completed blocks rendered, tail is plain
            parts.push_back(msg.stream_md_.build());
        } else {
            parts.push_back(markdown(msg.content));
        }
    }

    if (msg.streaming_) {
        parts.push_back(detail::hstack()(
            spinner_,
            Element{TextElement{
                .content = cfg_.streaming_text,
                .style = Style{}.with_dim(),
            }}
        ));
    }

    return detail::vstack()(std::move(parts));
}

Element ChatView::render_message(const ChatMessage& msg) const {
    // Streaming messages are never cached — they change every frame.
    if (msg.streaming_) {
        switch (msg.role) {
            case ChatRole::User:      return render_user_msg(msg);
            case ChatRole::Assistant: return render_assistant_msg(msg);
            case ChatRole::Tool:      return render_tool_msg(msg);
        }
        return {};
    }

    // Non-streaming messages use CachedElement which auto-invalidates
    // on resize via the RenderContext generation counter.
    return msg.cached_.get([&] {
        switch (msg.role) {
            case ChatRole::User:      return render_user_msg(msg);
            case ChatRole::Assistant: return render_assistant_msg(msg);
            case ChatRole::Tool:      return render_tool_msg(msg);
        }
        return Element{};
    });
}

// ============================================================================
// Build — assemble the complete chat UI
// ============================================================================

void ChatView::setup_input() {
    input_.on_submit([this](std::string_view text) {
        if (text.empty() || streaming_) return;
        add_user(text);
        input_.clear();
        if (on_submit_) on_submit_(text);
    });
}

Element ChatView::build() {
    // Lazy-init the input submit handler (can't do in constructor because
    // on_submit_ may not be set yet, and we capture `this`).
    if (!input_setup_) {
        setup_input();
        input_setup_ = true;
    }

    // Messages area
    Element msg_area;
    if (messages_.empty()) {
        msg_area = Element{TextElement{
            .content = "  " + cfg_.placeholder,
            .style = Style{}.with_dim().with_italic(),
        }};
    } else {
        std::vector<Element> rendered;
        rendered.reserve(messages_.size() * 2);
        for (auto& msg : messages_) {
            rendered.push_back(render_message(msg));
            rendered.push_back(Element{TextElement{.content = ""}});
        }
        msg_area = detail::vstack()(std::move(rendered));
    }

    // Divider
    auto divider = Element{TextElement{
        .content = cfg_.divider,
        .style = cfg_.divider_style,
    }};

    // Input prompt
    auto prompt = detail::hstack()(
        Element{TextElement{
            .content = cfg_.prompt_icon,
            .style = cfg_.prompt_style,
        }},
        input_
    );

    // Status bar — compact in narrow terminals
    std::string status_text;
    if (compact()) {
        status_text = streaming_ ? "\xe2\x97\x8f" : "\xe2\x97\x8f";  // just the dot
    } else {
        status_text = streaming_ ? cfg_.streaming_label : cfg_.ready_label;
    }
    auto status_left = Element{TextElement{
        .content = status_text,
        .style = streaming_ ? cfg_.status_streaming : cfg_.status_ready,
    }};
    auto status_right = Element{TextElement{
        .content = compact()
            ? std::to_string(messages_.size())
            : std::to_string(messages_.size()) + " msgs",
        .style = Style{}.with_dim(),
    }};
    auto status = detail::hstack()(
        std::move(status_left),
        detail::box().grow(1),
        std::move(status_right)
    );

    // Layout
    std::vector<Element> layout;
    layout.reserve(6);
    layout.push_back(std::move(msg_area));
    if (!toasts_.empty()) layout.push_back(toasts_.build());
    layout.push_back(std::move(divider));
    layout.push_back(std::move(prompt));
    layout.push_back(std::move(status));

    return detail::vstack()(std::move(layout));
}

} // namespace maya
