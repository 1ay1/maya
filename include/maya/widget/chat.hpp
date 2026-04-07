#pragma once
// maya::widget::chat — Complete AI chat UI widget
//
// Encapsulates everything needed for a Claude Code-style chat interface:
//   - Message model (User/Assistant/Tool roles)
//   - Progressive streaming markdown rendering
//   - Spinner animation during generation
//   - Tool call badges with structured data display
//   - Toast notifications
//   - Text input with focus management
//   - Status line (streaming/ready, message count)
//
// Usage:
//   ChatView chat;
//   chat.on_submit([&](std::string_view text) {
//       chat.begin_stream();
//       // ... feed from LLM ...
//       chat.stream_append("Hello **world**");
//       chat.end_stream();
//   });
//
//   // In event handler:
//   chat.handle(key_event);
//
//   // In render:
//   chat.advance(dt);
//   return chat.build();

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/render_context.hpp"
#include "../element/builder.hpp"
#include "../style/style.hpp"
#include "badge.hpp"
#include "input.hpp"
#include "markdown.hpp"
#include "spinner.hpp"
#include "table.hpp"
#include "toast.hpp"

namespace maya {

// ============================================================================
// ChatRole — message sender identity
// ============================================================================

enum class ChatRole : uint8_t { User, Assistant, Tool };

// ============================================================================
// ChatMessage — a single message in the conversation
// ============================================================================

struct ChatMessage {
    ChatRole    role;
    std::string content;
    std::string tool_name;           // only for ChatRole::Tool

    // Internal rendering state (mutable for logical constness in build).
    // CachedElement auto-invalidates on resize via the RenderContext
    // generation counter — no manual invalidation needed.
    mutable CachedElement       cached_;
    mutable StreamingMarkdown   stream_md_;
    bool streaming_ = false;
};

// ============================================================================
// ChatViewConfig — visual and behavioral configuration
// ============================================================================

struct ChatViewConfig {
    // Prompt
    std::string prompt_icon    = "\xe2\x9d\xaf ";  // "❯ "
    Style prompt_style         = Style{}.with_bold().with_fg(Color::rgb(100, 200, 255));
    std::string placeholder    = "Type a message...";

    // User messages
    Style user_style           = Style{}.with_fg(Color::rgb(220, 220, 240));

    // Assistant messages
    std::string assistant_label = "\xe2\x97\x86 Assistant";  // "◆ Assistant"
    Style assistant_label_style = Style{}.with_bold().with_fg(Color::rgb(180, 140, 255));
    std::string streaming_text = " Generating...";

    // Status bar
    Style status_streaming     = Style{}.with_fg(Color::rgb(255, 200, 60));
    Style status_ready         = Style{}.with_fg(Color::rgb(80, 220, 120));
    std::string streaming_label = "\xe2\x97\x8f streaming";  // "● streaming"
    std::string ready_label     = "\xe2\x97\x8f ready";       // "● ready"

    // Divider
    std::string divider        = "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";  // "───"
    Style divider_style        = Style{}.with_dim();

    // Toast
    ToastConfig toast_config   = {};

    // Tool messages
    Style tool_header_style    = Style{}.with_bold().with_dim();
    Style tool_border_style    = Style{}.with_fg(Color::rgb(50, 50, 70));
};

// ============================================================================
// ChatView — complete AI chat interface widget
// ============================================================================

class ChatView {
    std::vector<ChatMessage> messages_;
    Input<>                  input_;
    Spinner<SpinnerStyle::Dots> spinner_;
    ToastManager             toasts_;
    ChatViewConfig           cfg_;
    bool                     streaming_ = false;
    bool                     input_setup_ = false;

    std::move_only_function<void(std::string_view)> on_submit_;
    std::move_only_function<Element(const ChatMessage&) const> render_tool_fn_;

public:
    ChatView() { input_.set_placeholder(cfg_.placeholder); }

    explicit ChatView(ChatViewConfig cfg)
        : cfg_(std::move(cfg))
    {
        input_.set_placeholder(cfg_.placeholder);
    }

    // -- Focus integration --------------------------------------------------

    [[nodiscard]] FocusNode& focus_node() { return input_.focus_node(); }

    // -- Message management -------------------------------------------------

    /// Add a user message to the conversation.
    void add_user(std::string_view content) {
        messages_.push_back({ChatRole::User, std::string{content}, {}, {}, {}, false});
    }

    /// Add a tool result message.
    void add_tool(std::string_view tool_name, std::string_view content) {
        messages_.push_back({ChatRole::Tool, std::string{content},
                            std::string{tool_name}, {}, {}, false});
    }

    // -- Streaming API ------------------------------------------------------

    /// Start streaming a new assistant message.
    void begin_stream() {
        streaming_ = true;
        messages_.push_back({ChatRole::Assistant, {}, {}, {}, {}, true});
    }

    /// Set the full content of the current streaming message (cumulative).
    void stream_set(std::string_view content) {
        if (!streaming_ || messages_.empty()) return;
        auto& msg = messages_.back();
        msg.content = std::string{content};
        msg.stream_md_.set_content(content);
    }

    /// Append text to the current streaming message (incremental).
    void stream_append(std::string_view text) {
        if (!streaming_ || messages_.empty()) return;
        auto& msg = messages_.back();
        msg.content += text;
        msg.stream_md_.append(text);
    }

    /// Finalize the current streaming message — parses remaining markdown.
    void end_stream() {
        if (!streaming_ || messages_.empty()) return;
        auto& msg = messages_.back();
        msg.stream_md_.finish();
        msg.streaming_ = false;
        streaming_ = false;
    }

    [[nodiscard]] bool is_streaming() const noexcept { return streaming_; }

    // -- Toast notifications ------------------------------------------------

    void toast(std::string_view message, ToastLevel level = ToastLevel::Info) {
        toasts_.push(message, level);
    }

    // -- Callbacks ----------------------------------------------------------

    /// Set the callback for when the user submits a message.
    /// ChatView automatically adds the user message and clears the input.
    template <std::invocable<std::string_view> F>
    void on_submit(F&& fn) { on_submit_ = std::forward<F>(fn); }

    /// Override the default tool message renderer.
    template <typename F>
        requires std::invocable<F, const ChatMessage&> &&
                 std::convertible_to<std::invoke_result_t<F, const ChatMessage&>, Element>
    void on_render_tool(F&& fn) { render_tool_fn_ = std::forward<F>(fn); }

    // -- Timing -------------------------------------------------------------

    /// Advance animations (spinner, toasts). Call once per frame.
    void advance(float dt) {
        spinner_.advance(dt);
        toasts_.advance(dt);
    }

    // -- Event handling -----------------------------------------------------

    /// Handle a key event. Returns true if consumed.
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        return input_.handle(ev);
    }

    // -- Rendering ----------------------------------------------------------

    /// Build the complete chat UI element tree.
    [[nodiscard]] Element build();

    operator Element() { return build(); }

    // -- Resize / responsive ------------------------------------------------
    // Geometry is managed automatically via the RenderContext: widgets
    // query available_width() / available_height() during build(), and
    // CachedElement auto-invalidates when the render generation changes.
    // No manual resize() call is required.

    /// Manual resize override (for headless testing or non-App usage).
    /// When using App, this is unnecessary — the framework propagates size.
    void resize(int /*width*/, int /*height*/) {
        invalidate_cache();
    }

    /// True when the terminal is narrow enough to warrant compact labels.
    [[nodiscard]] bool compact() const noexcept { return available_width() < 60; }

    [[nodiscard]] int term_width()  const noexcept { return available_width(); }
    [[nodiscard]] int term_height() const noexcept { return available_height(); }

    // -- Accessors ----------------------------------------------------------

    [[nodiscard]] const std::vector<ChatMessage>& messages() const noexcept { return messages_; }
    [[nodiscard]] int message_count() const noexcept { return static_cast<int>(messages_.size()); }

    /// Clear all messages and reset state.
    void clear() {
        messages_.clear();
        streaming_ = false;
        toasts_.clear();
    }

    /// Invalidate all cached message renders (e.g. after resize).
    void invalidate_cache() {
        for (auto& msg : messages_) {
            msg.cached_.reset();
            msg.stream_md_.clear();
        }
    }

private:
    void setup_input();
    [[nodiscard]] Element render_user_msg(const ChatMessage& msg) const;
    [[nodiscard]] Element render_assistant_msg(const ChatMessage& msg) const;
    [[nodiscard]] Element render_tool_msg(const ChatMessage& msg) const;
    [[nodiscard]] Element render_message(const ChatMessage& msg) const;
};

} // namespace maya
