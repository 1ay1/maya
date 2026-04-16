#pragma once
// maya::widget::calendar — Month calendar grid
//
// Interactive month view with arrow-key navigation and day selection.
//
//        April 2026
//   Mo Tu We Th Fr Sa Su
//          1  2  3  4  5
//    6  7  8  9 10 11 12
//   13 14 15 16 17 18 19
//   20 21 22 23 24 25 26
//   27 28 29 30
//
// Usage:
//   Calendar cal(2026, 4);
//   cal.on_select([](int y, int m, int d) { ... });
//   auto ui = cal.build();

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

class Calendar {
    int year_;
    int month_;  // 1-12
    Signal<int> selected_day_{1};
    FocusNode focus_;

    std::move_only_function<void(int, int, int)> on_select_;

    // Today's date (set at construction for highlighting)
    int today_year_  = 0;
    int today_month_ = 0;
    int today_day_   = 0;

    // -- Date utilities --
    [[nodiscard]] static bool is_leap_year(int y) {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    }

    [[nodiscard]] static int days_in_month(int y, int m) {
        static constexpr int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (m == 2 && is_leap_year(y)) return 29;
        return days[m];
    }

    // Day of week for the 1st of the month (0=Monday, 6=Sunday)
    // Using Tomohiko Sakamoto's algorithm
    [[nodiscard]] static int day_of_week(int y, int m, int d) {
        static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        if (m < 3) y -= 1;
        int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
        // Convert: 0=Sunday -> 6, 1=Monday -> 0, ...
        return (dow + 6) % 7;
    }

    [[nodiscard]] static const char* month_name(int m) {
        static constexpr const char* names[] = {
            "", "January", "February", "March", "April",
            "May", "June", "July", "August",
            "September", "October", "November", "December"
        };
        return names[m];
    }

public:
    Calendar(int year, int month)
        : year_(year), month_(std::clamp(month, 1, 12)) {}

    Calendar(int year, int month, int today_y, int today_m, int today_d)
        : year_(year), month_(std::clamp(month, 1, 12)),
          today_year_(today_y), today_month_(today_m), today_day_(today_d) {}

    // -- Accessors --
    [[nodiscard]] FocusNode&       focus_node()       { return focus_; }
    [[nodiscard]] const FocusNode& focus_node() const { return focus_; }
    [[nodiscard]] const Signal<int>& selected_day()   const { return selected_day_; }
    [[nodiscard]] int year()  const { return year_; }
    [[nodiscard]] int month() const { return month_; }

    void set_today(int y, int m, int d) {
        today_year_ = y; today_month_ = m; today_day_ = d;
    }

    // -- Navigation --
    void next_month() {
        if (month_ == 12) { month_ = 1; year_++; }
        else month_++;
        int dim = days_in_month(year_, month_);
        if (selected_day_() > dim) selected_day_.set(dim);
    }

    void prev_month() {
        if (month_ == 1) { month_ = 12; year_--; }
        else month_--;
        int dim = days_in_month(year_, month_);
        if (selected_day_() > dim) selected_day_.set(dim);
    }

    // -- Callback --
    template <std::invocable<int, int, int> F>
    void on_select(F&& fn) { on_select_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        int dim = days_in_month(year_, month_);

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                int day = selected_day_();
                switch (sk) {
                    case SpecialKey::Left:
                        if (day > 1) selected_day_.set(day - 1);
                        return true;
                    case SpecialKey::Right:
                        if (day < dim) selected_day_.set(day + 1);
                        return true;
                    case SpecialKey::Up:
                        if (day > 7) selected_day_.set(day - 7);
                        else selected_day_.set(1);
                        return true;
                    case SpecialKey::Down:
                        if (day + 7 <= dim) selected_day_.set(day + 7);
                        else selected_day_.set(dim);
                        return true;
                    case SpecialKey::Enter:
                        if (on_select_) on_select_(year_, month_, day);
                        return true;
                    default: return false;
                }
            },
            [&](CharKey) -> bool {
                return false;
            },
        }, ev.key);
    }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        int dim = days_in_month(year_, month_);
        int first_dow = day_of_week(year_, month_, 1);  // 0=Mon
        int sel = selected_day_();
        bool focused = focus_.focused();
        bool is_today_month = (year_ == today_year_ && month_ == today_month_);

        auto header_style = Style{}.with_bold();
        auto weekday_style = Style{}.with_dim();
        auto day_style = Style{};
        auto weekend_style = Style{}.with_dim();
        auto today_style = Style{}.with_fg(Color::blue()).with_bold();
        auto selected_style = focused
            ? Style{}.with_inverse().with_bold()
            : Style{}.with_bold().with_fg(Color::blue());

        std::vector<Element> rows;

        // Title: "    April 2026"
        std::string title = std::string(month_name(month_)) + " " + std::to_string(year_);
        // Center the title over 20 chars (7 columns * 3 chars each ~= 20)
        int title_pad = std::max(0, (20 - static_cast<int>(title.size())) / 2);
        std::string title_line(static_cast<size_t>(title_pad), ' ');
        title_line += title;

        rows.push_back(Element{TextElement{
            .content = std::move(title_line),
            .style = header_style,
            .wrap = TextWrap::NoWrap,
        }});

        // Weekday header: "Mo Tu We Th Fr Sa Su"
        rows.push_back(Element{TextElement{
            .content = "Mo Tu We Th Fr Sa Su",
            .style = weekday_style,
            .wrap = TextWrap::NoWrap,
        }});

        // Day grid — each cell is 2 chars + 1 space separator = 3 chars per column
        int day = 1;
        while (day <= dim) {
            std::string line;
            std::vector<StyledRun> runs;

            for (int col = 0; col < 7; ++col) {
                // Separator between columns
                if (col > 0) line += ' ';

                if ((day == 1 && col < first_dow) || day > dim) {
                    // Empty cell: 2 spaces
                    line += "  ";
                } else {
                    // Format day number right-aligned in 2 chars
                    std::string cell = (day < 10)
                        ? " " + std::to_string(day)
                        : std::to_string(day);

                    // Pick style
                    bool is_weekend = (col >= 5);
                    bool is_today = is_today_month && (day == today_day_);
                    bool is_selected = (day == sel);

                    Style s = day_style;
                    if (is_selected)     s = selected_style;
                    else if (is_today)   s = today_style;
                    else if (is_weekend) s = weekend_style;

                    size_t start = line.size();
                    line += cell;
                    runs.push_back(StyledRun{start, cell.size(), s});
                    day++;
                }
            }

            rows.push_back(Element{TextElement{
                .content = std::move(line),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        return dsl::v(std::move(rows)).build();
    }
};

} // namespace maya
