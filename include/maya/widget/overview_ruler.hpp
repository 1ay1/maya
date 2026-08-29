#pragma once
// maya::widget::OverviewRuler — document overview scrollbar (background-free)
//
// The thin ruler down the right edge of an editor: a viewport thumb plus
// coloured tick marks for diagnostics, search hits, and changes, positioned by
// their line within the whole document. Give it the line count, the visible
// range, the cursor line, and a set of marks; it scales them to its height.
//
// Foreground-only — the terminal keeps its background.
//
// Usage:
//   OverviewRuler r;
//   r.total(420).viewport(120, 168).cursor(140)
//    .mark(35, RulerMark::Error).mark(212, RulerMark::Search);
//   Element ui = r | dsl::width(2) | dsl::height(30);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

enum class RulerMark : uint8_t { Error, Warning, Info, Search, Change };

struct OverviewRulerTheme {
    Color thumb  = Color::hex(0x45475A); // viewport thumb bar
    Color cursor = Color::hex(0x89B4FA); // cursor position
    Color error  = Color::hex(0xF38BA8);
    Color warn   = Color::hex(0xF9E2AF);
    Color info   = Color::hex(0x89B4FA);
    Color search = Color::hex(0xF9E2AF);
    Color change = Color::hex(0xA6E3A1);
};

struct OverviewRulerConfig {
    int                width = 2; // columns: col 0 = thumb, rest = marker ticks
    OverviewRulerTheme theme = {};
};

class OverviewRuler {
public:
    OverviewRuler() = default;
    explicit OverviewRuler(OverviewRulerConfig cfg) : cfg_(cfg) {}

    OverviewRuler& total(int lines)             { total_ = std::max(1, lines); return *this; }
    OverviewRuler& viewport(int top, int bot)   { vtop_ = top; vbot_ = bot; return *this; }
    OverviewRuler& cursor(int line)             { cursor_ = line; return *this; }
    OverviewRuler& mark(int line, RulerMark m)  { marks_.push_back({line, m}); return *this; }
    OverviewRuler& clear_marks()                { marks_.clear(); return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        return Element{ComponentElement{
            .render = [self = *this](int w, int h) -> Element { return self.paint(w, h); },
            .measure = [w = cfg_.width](int) -> Size { return Size{Columns(w), Rows(1)}; },
        }};
    }

private:
    OverviewRulerConfig                  cfg_{};
    int                                  total_  = 1;
    int                                  vtop_   = -1;
    int                                  vbot_   = -1;
    int                                  cursor_ = -1;
    std::vector<std::pair<int, RulerMark>> marks_;

    int rank(RulerMark m) const { // higher wins when several land on one row
        switch (m) {
            case RulerMark::Error:  return 5;
            case RulerMark::Warning:return 4;
            case RulerMark::Search: return 3;
            case RulerMark::Change: return 2;
            case RulerMark::Info:   return 1;
        }
        return 0;
    }
    Color mark_color(RulerMark m) const {
        switch (m) {
            case RulerMark::Error:  return cfg_.theme.error;
            case RulerMark::Warning:return cfg_.theme.warn;
            case RulerMark::Info:   return cfg_.theme.info;
            case RulerMark::Search: return cfg_.theme.search;
            case RulerMark::Change: return cfg_.theme.change;
        }
        return cfg_.theme.info;
    }

    Element paint(int w, int h) const {
        const int cols = std::clamp(std::min(w, cfg_.width), 1, std::max(1, w));

        std::vector<Element> rows;
        rows.reserve(static_cast<size_t>(h));
        for (int r = 0; r < h; ++r) {
            // line range this row represents
            int l0 = static_cast<int>(static_cast<long long>(r) * total_ / h) + 1;
            int l1 = static_cast<int>(static_cast<long long>(r + 1) * total_ / h);
            l1 = std::max(l1, l0);

            const bool thumb  = (vtop_ > 0 && l1 >= vtop_ && l0 <= vbot_);
            const bool curs   = (cursor_ >= l0 && cursor_ <= l1);

            bool has_mark = false; RulerMark best{}; int best_rank = 0;
            for (auto [ml, mk] : marks_)
                if (ml >= l0 && ml <= l1 && rank(mk) > best_rank) {
                    best = mk; best_rank = rank(mk); has_mark = true;
                }

            std::string content; std::vector<StyledRun> runs;
            auto cell = [&](const char* g, Style st) {
                const size_t off = content.size();
                content += g;
                runs.push_back({off, std::string_view(g).size(), st});
            };

            for (int c = 0; c < cols; ++c) {
                const bool thumb_col  = (c == 0 && cols > 1);
                const bool marker_col = !thumb_col;
                if (cols == 1) {
                    // single column: mark > cursor > thumb > track
                    if (has_mark)   cell("\xe2\x96\x90", Style{}.with_fg(mark_color(best)).with_bold());
                    else if (curs)  cell("\xe2\x96\xb6", Style{}.with_fg(cfg_.theme.cursor));
                    else if (thumb) cell("\xe2\x96\x90", Style{}.with_fg(cfg_.theme.thumb));
                    else            cell(" ", Style{});
                } else if (thumb_col) {
                    cell(thumb ? "\xe2\x96\x8a" : " ",       // ▊ thumb
                         Style{}.with_fg(cfg_.theme.thumb));
                } else if (marker_col) {
                    if (has_mark)  cell("\xe2\x96\xac", Style{}.with_fg(mark_color(best))); // ▬
                    else if (curs) cell("\xe2\x96\xb6", Style{}.with_fg(cfg_.theme.cursor)); // ▶
                    else           cell(" ", Style{});
                }
            }
            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = Style{},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }
        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
