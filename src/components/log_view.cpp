#include "maya/components/log_view.hpp"

namespace maya::components {

Element parse_ansi_line(std::string_view line) {
    using namespace maya::dsl;

    struct AnsiState {
        std::optional<Color> fg{};
        std::optional<Color> bg{};
        bool bold      = false;
        bool dim       = false;
        bool italic    = false;
        bool underline = false;

        [[nodiscard]] Style to_style() const {
            Style s;
            if (fg) s = s.with_fg(*fg);
            if (bg) s = s.with_bg(*bg);
            if (bold)      s = s.with_bold();
            if (dim)       s = s.with_dim();
            if (italic)    s = s.with_italic();
            if (underline) s = s.with_underline();
            return s;
        }

        void reset() {
            fg = std::nullopt;
            bg = std::nullopt;
            bold = dim = italic = underline = false;
        }
    };

    // Map 0-7 index to AnsiColor for standard colors
    auto index_to_ansi = [](int idx) -> AnsiColor {
        return static_cast<AnsiColor>(idx);
    };

    // Map 0-7 index to bright AnsiColor
    auto index_to_bright = [](int idx) -> AnsiColor {
        return static_cast<AnsiColor>(8 + idx);
    };

    std::vector<Element> segments;
    AnsiState state;
    std::size_t pos = 0;
    std::size_t text_start = 0;

    auto flush_text = [&](std::size_t end) {
        if (end > text_start) {
            auto chunk = std::string{line.substr(text_start, end - text_start)};
            segments.push_back(text(std::move(chunk), state.to_style()).build());
        }
    };

    while (pos < line.size()) {
        // Look for ESC [ sequence
        if (line[pos] == '\033' && pos + 1 < line.size() && line[pos + 1] == '[') {
            flush_text(pos);

            // Find the end of the escape sequence
            std::size_t seq_start = pos + 2;
            std::size_t seq_end = seq_start;
            while (seq_end < line.size() &&
                   ((line[seq_end] >= '0' && line[seq_end] <= '9') || line[seq_end] == ';'))
                ++seq_end;

            if (seq_end < line.size() && line[seq_end] == 'm') {
                // SGR sequence — parse parameters
                auto params_str = line.substr(seq_start, seq_end - seq_start);

                // Parse semicolon-separated integers
                std::vector<int> params;
                if (params_str.empty()) {
                    params.push_back(0); // bare ESC[m is reset
                } else {
                    std::size_t p = 0;
                    while (p < params_str.size()) {
                        int val = 0;
                        auto [ptr, ec] = std::from_chars(
                            params_str.data() + p,
                            params_str.data() + params_str.size(),
                            val);
                        if (ec != std::errc{}) {
                            // skip unparseable
                            ++p;
                            continue;
                        }
                        params.push_back(val);
                        p = static_cast<std::size_t>(ptr - params_str.data());
                        if (p < params_str.size() && params_str[p] == ';') ++p;
                    }
                }

                // Interpret SGR parameters
                for (std::size_t i = 0; i < params.size(); ++i) {
                    int code = params[i];

                    if (code == 0) {
                        state.reset();
                    } else if (code == 1) {
                        state.bold = true;
                    } else if (code == 2) {
                        state.dim = true;
                    } else if (code == 3) {
                        state.italic = true;
                    } else if (code == 4) {
                        state.underline = true;
                    } else if (code == 22) {
                        state.bold = false;
                        state.dim = false;
                    } else if (code == 23) {
                        state.italic = false;
                    } else if (code == 24) {
                        state.underline = false;
                    } else if (code >= 30 && code <= 37) {
                        state.fg = Color(index_to_ansi(code - 30));
                    } else if (code == 38 && i + 1 < params.size()) {
                        if (params[i + 1] == 5 && i + 2 < params.size()) {
                            // 256-color: 38;5;N
                            state.fg = Color::indexed(static_cast<uint8_t>(params[i + 2]));
                            i += 2;
                        } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                            // Truecolor: 38;2;R;G;B
                            state.fg = Color::rgb(
                                static_cast<uint8_t>(params[i + 2]),
                                static_cast<uint8_t>(params[i + 3]),
                                static_cast<uint8_t>(params[i + 4]));
                            i += 4;
                        }
                    } else if (code == 39) {
                        state.fg = std::nullopt;
                    } else if (code >= 40 && code <= 47) {
                        state.bg = Color(index_to_ansi(code - 40));
                    } else if (code == 48 && i + 1 < params.size()) {
                        if (params[i + 1] == 5 && i + 2 < params.size()) {
                            // 256-color: 48;5;N
                            state.bg = Color::indexed(static_cast<uint8_t>(params[i + 2]));
                            i += 2;
                        } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                            // Truecolor: 48;2;R;G;B
                            state.bg = Color::rgb(
                                static_cast<uint8_t>(params[i + 2]),
                                static_cast<uint8_t>(params[i + 3]),
                                static_cast<uint8_t>(params[i + 4]));
                            i += 4;
                        }
                    } else if (code == 49) {
                        state.bg = std::nullopt;
                    } else if (code >= 90 && code <= 97) {
                        state.fg = Color(index_to_bright(code - 90));
                    } else if (code >= 100 && code <= 107) {
                        state.bg = Color(index_to_bright(code - 100));
                    }
                }

                pos = seq_end + 1;
            } else {
                // Non-SGR escape sequence — skip to the terminating byte
                // (any byte in 0x40-0x7E ends a CSI sequence)
                while (seq_end < line.size() && line[seq_end] < 0x40) ++seq_end;
                if (seq_end < line.size()) ++seq_end; // skip the final byte
                pos = seq_end;
            }
            text_start = pos;
        } else {
            ++pos;
        }
    }

    flush_text(line.size());

    if (segments.empty()) {
        return text("").build();
    }
    if (segments.size() == 1) {
        return std::move(segments[0]);
    }
    return hstack()(std::move(segments));
}

void LogView::clamp() {
    int max_off = std::max(0, static_cast<int>(lines_.size()) - props_.max_visible);
    offset_ = std::clamp(offset_, 0, max_off);
}

void LogView::append(std::string line) {
    bool was_at_bottom = at_bottom();
    lines_.push_back(std::move(line));
    while (static_cast<int>(lines_.size()) > props_.max_lines) {
        lines_.pop_front();
        if (offset_ > 0) --offset_;
    }
    if (following_ && was_at_bottom) {
        offset_ = std::max(0, static_cast<int>(lines_.size()) - props_.max_visible);
    }
    clamp();
}

void LogView::append_lines(std::vector<std::string> lines) {
    for (auto& l : lines) append(std::move(l));
}

void LogView::clear() {
    lines_.clear();
    offset_ = 0;
    following_ = props_.tail_follow;
}

bool LogView::at_bottom() const {
    return offset_ >= static_cast<int>(lines_.size()) - props_.max_visible;
}

bool LogView::update(const Event& ev) {
    if (scrolled_up(ev) || key(ev, SpecialKey::Up)) {
        offset_--; following_ = false; clamp(); return true;
    }
    if (scrolled_down(ev) || key(ev, SpecialKey::Down)) {
        offset_++; clamp();
        if (at_bottom()) following_ = true;
        return true;
    }
    if (key(ev, SpecialKey::PageUp)) {
        offset_ -= props_.max_visible; following_ = false; clamp(); return true;
    }
    if (key(ev, SpecialKey::PageDown)) {
        offset_ += props_.max_visible; clamp();
        if (at_bottom()) following_ = true;
        return true;
    }
    if (ctrl(ev, 'a')) {
        offset_ = 0; following_ = false; return true;
    }
    if (ctrl(ev, 'e')) {
        offset_ = std::max(0, static_cast<int>(lines_.size()) - props_.max_visible);
        following_ = true;
        return true;
    }
    return false;
}

Element LogView::render() const {
    using namespace maya::dsl;

    int total = static_cast<int>(lines_.size());
    int vis_start = offset_;
    int vis_end   = std::min(total, offset_ + props_.max_visible);

    // Width of line number gutter (for alignment)
    int num_width = 0;
    if (props_.show_line_nums && total > 0) {
        int max_num = total;
        while (max_num > 0) { ++num_width; max_num /= 10; }
    }

    std::vector<Element> rows;
    rows.reserve(vis_end - vis_start);

    for (int i = vis_start; i < vis_end; ++i) {
        auto line_elem = parse_ansi_line(lines_[static_cast<std::size_t>(i)]);

        if (props_.show_line_nums) {
            // Right-aligned, dim line number
            auto num_str = std::to_string(i + 1);
            while (static_cast<int>(num_str.size()) < num_width)
                num_str.insert(num_str.begin(), ' ');

            auto num_elem = text(std::move(num_str),
                Style{}.with_dim().with_fg(palette().muted)).build();

            rows.push_back(
                hstack()(std::move(num_elem),
                         text(" ", Style{}).build(),
                         std::move(line_elem)));
        } else {
            rows.push_back(std::move(line_elem));
        }
    }

    // Scrollbar
    if (props_.show_scrollbar && total > props_.max_visible) {
        float ratio = static_cast<float>(offset_) /
                      static_cast<float>(std::max(1, total - props_.max_visible));
        int bar_h   = std::max(1, props_.max_visible * props_.max_visible / std::max(1, total));
        int bar_pos = static_cast<int>(ratio * static_cast<float>(props_.max_visible - bar_h));

        std::vector<Element> with_scrollbar;
        with_scrollbar.reserve(rows.size());

        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            bool in_thumb = (i >= bar_pos && i < bar_pos + bar_h);
            auto indicator = text(in_thumb ? "┃" : "│",
                Style{}.with_fg(in_thumb ? palette().primary : palette().dim)).build();

            with_scrollbar.push_back(
                hstack()(std::move(rows[static_cast<std::size_t>(i)]),
                         Element(space),
                         std::move(indicator)));
        }
        return vstack()(std::move(with_scrollbar));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
