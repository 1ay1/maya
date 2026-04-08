#pragma once
// maya::widget::log_viewer — Streaming log viewer with scroll and filtering
//
// Displays timestamped, level-colored log entries with scroll windowing.
// Auto-scrolls to bottom on new entries unless the user has scrolled up.
//
//   2026-04-09 12:00:01  [INFO]   Server started on port 8080
//   2026-04-09 12:00:02  [WARN]   Cache miss for key "user.42"
//   2026-04-09 12:00:03  [ERROR]  Connection refused: 10.0.0.5
//
// Usage:
//   LogViewer viewer;
//   viewer.push({.timestamp = "12:00:01", .message = "Started", .level = LogLevel::Info});
//   auto ui = viewer.build();

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../dsl.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// LogLevel — severity levels
// ============================================================================

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
};

// ============================================================================
// LogEntry — a single log record
// ============================================================================

struct LogEntry {
    std::string timestamp;
    std::string message;
    LogLevel    level = LogLevel::Info;
};

// ============================================================================
// LogViewer — streaming log viewer widget
// ============================================================================

class LogViewer {
    std::vector<LogEntry> entries_;
    int max_entries_ = 1000;
    int visible_     = 20;
    int scroll_offset_ = 0;
    bool auto_scroll_  = true;

    FocusNode focus_;

    // Filter state
    bool filter_active_ = false;
    LogLevel filter_level_ = LogLevel::Debug;  // show this level and above

    [[nodiscard]] static const char* level_tag(LogLevel lv) {
        switch (lv) {
            case LogLevel::Debug: return "[DEBUG]";
            case LogLevel::Info:  return "[INFO] ";
            case LogLevel::Warn:  return "[WARN] ";
            case LogLevel::Error: return "[ERROR]";
        }
        return "[?????]";
    }

    [[nodiscard]] static Style level_style(LogLevel lv) {
        switch (lv) {
            case LogLevel::Debug: return Style{}.with_fg(Color::rgb(92, 99, 112));
            case LogLevel::Info:  return Style{}.with_fg(Color::rgb(97, 175, 239));
            case LogLevel::Warn:  return Style{}.with_fg(Color::rgb(229, 192, 123));
            case LogLevel::Error: return Style{}.with_fg(Color::rgb(224, 108, 117)).with_bold();
        }
        return Style{}.with_fg(Color::rgb(150, 156, 170));
    }

    [[nodiscard]] std::vector<int> filtered_indices() const {
        std::vector<int> out;
        out.reserve(entries_.size());
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (!filter_active_ ||
                static_cast<uint8_t>(entries_[static_cast<size_t>(i)].level) >=
                    static_cast<uint8_t>(filter_level_)) {
                out.push_back(i);
            }
        }
        return out;
    }

    void clamp_scroll(int total) {
        int max_off = std::max(0, total - visible_);
        scroll_offset_ = std::clamp(scroll_offset_, 0, max_off);
    }

public:
    LogViewer() = default;

    explicit LogViewer(int visible, int max_entries = 1000)
        : max_entries_(max_entries), visible_(visible) {}

    // -- Accessors --
    [[nodiscard]] FocusNode&       focus_node()       { return focus_; }
    [[nodiscard]] const FocusNode& focus_node() const { return focus_; }
    [[nodiscard]] int              entry_count() const { return static_cast<int>(entries_.size()); }
    [[nodiscard]] bool             auto_scroll() const { return auto_scroll_; }

    // -- Mutation --
    void push(LogEntry entry) {
        entries_.push_back(std::move(entry));
        // Trim to max
        if (static_cast<int>(entries_.size()) > max_entries_) {
            int excess = static_cast<int>(entries_.size()) - max_entries_;
            entries_.erase(entries_.begin(), entries_.begin() + excess);
            scroll_offset_ = std::max(0, scroll_offset_ - excess);
        }
        // Auto-scroll
        if (auto_scroll_) {
            auto vis = filtered_indices();
            int total = static_cast<int>(vis.size());
            scroll_offset_ = std::max(0, total - visible_);
        }
    }

    void clear() {
        entries_.clear();
        scroll_offset_ = 0;
        auto_scroll_ = true;
    }

    void set_visible(int n) { visible_ = n; }
    void set_max_entries(int n) { max_entries_ = n; }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                auto vis = filtered_indices();
                int total = static_cast<int>(vis.size());
                switch (sk) {
                    case SpecialKey::Up:
                        scroll_offset_ = std::max(0, scroll_offset_ - 1);
                        auto_scroll_ = false;
                        return true;
                    case SpecialKey::Down: {
                        int max_off = std::max(0, total - visible_);
                        scroll_offset_ = std::min(max_off, scroll_offset_ + 1);
                        if (scroll_offset_ >= max_off) auto_scroll_ = true;
                        return true;
                    }
                    case SpecialKey::PageUp:
                        scroll_offset_ = std::max(0, scroll_offset_ - visible_);
                        auto_scroll_ = false;
                        return true;
                    case SpecialKey::PageDown: {
                        int max_off = std::max(0, total - visible_);
                        scroll_offset_ = std::min(max_off, scroll_offset_ + visible_);
                        if (scroll_offset_ >= max_off) auto_scroll_ = true;
                        return true;
                    }
                    case SpecialKey::Home:
                        scroll_offset_ = 0;
                        auto_scroll_ = false;
                        return true;
                    case SpecialKey::End: {
                        int max_off = std::max(0, total - visible_);
                        scroll_offset_ = max_off;
                        auto_scroll_ = true;
                        return true;
                    }
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'f') {
                    filter_active_ = !filter_active_;
                    scroll_offset_ = 0;
                    return true;
                }
                return false;
            },
        }, ev.key);
    }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto vis = filtered_indices();
        int total = static_cast<int>(vis.size());

        if (total == 0) {
            return Element{TextElement{
                .content = "  No log entries",
                .style = Style{}.with_fg(Color::rgb(92, 99, 112)),
            }};
        }

        int start = std::clamp(scroll_offset_, 0, std::max(0, total - visible_));
        int count = std::min(visible_, total - start);

        auto ts_style = Style{}.with_fg(Color::rgb(150, 156, 170));
        auto msg_style = Style{}.with_fg(Color::rgb(200, 204, 212));

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(count) + 1);

        for (int i = start; i < start + count; ++i) {
            const auto& entry = entries_[static_cast<size_t>(vis[static_cast<size_t>(i)])];

            std::string line;
            std::vector<StyledRun> runs;

            // Timestamp
            size_t ts_start = line.size();
            line += entry.timestamp;
            runs.push_back(StyledRun{ts_start, entry.timestamp.size(), ts_style});

            line += "  ";

            // Level tag
            const char* tag = level_tag(entry.level);
            size_t tag_start = line.size();
            std::string tag_str{tag};
            line += tag_str;
            runs.push_back(StyledRun{tag_start, tag_str.size(), level_style(entry.level)});

            line += "  ";

            // Message
            size_t msg_start = line.size();
            line += entry.message;
            runs.push_back(StyledRun{msg_start, entry.message.size(), msg_style});

            rows.push_back(Element{TextElement{
                .content = std::move(line),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Status line: scroll position + filter info
        std::string status;
        if (filter_active_) {
            status += "\xe2\x96\xb8 filtered  ";  // "▸ filtered  "
        }
        status += std::to_string(start + 1) + "-" +
                  std::to_string(start + count) + "/" +
                  std::to_string(total);
        if (auto_scroll_) status += "  [auto]";

        rows.push_back(Element{TextElement{
            .content = std::move(status),
            .style = Style{}.with_fg(Color::rgb(92, 99, 112)),
        }});

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
