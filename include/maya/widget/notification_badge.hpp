#pragma once
// maya::widget::NotificationBadge — labelled count badge (background-free)
//
// A label with a small coloured count pill — the "Problems 3" / bell-with-count
// indicators. The count uses reverse-video (terminal's own colours) so it reads
// as a solid badge on any background.
//
// Usage:  NotificationBadge{}.icon("\uf0f3").label("Notifications").count(5);

#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"

namespace maya {

struct NotificationBadge {
    std::string icon_;
    std::string label_;
    int         count_ = 0;
    Color       label_color = Color::hex(0xBAC2DE);
    Color       icon_color  = Color::hex(0x89B4FA);
    bool        error = false;

    NotificationBadge& icon(std::string s)  { icon_ = std::move(s); return *this; }
    NotificationBadge& label(std::string s) { label_ = std::move(s); return *this; }
    NotificationBadge& count(int n)         { count_ = n; return *this; }
    NotificationBadge& as_error(bool v = true) { error = v; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::string s; std::vector<StyledRun> r;
        auto put=[&](std::string_view t, Style st){ if(t.empty())return;
            r.push_back({s.size(),t.size(),st}); s+=t; };
        if (!icon_.empty()) put(icon_ + " ", Style{}.with_fg(icon_color));
        if (!label_.empty()) put(label_, Style{}.with_fg(label_color));
        if (count_ > 0) {
            std::string pill = " " + std::to_string(count_) + " ";
            Style bs = Style{}.with_inverse().with_bold();
            if (!label_.empty()) put(" ", Style{});
            put(pill, bs);
        }
        return Element{TextElement{ .content=std::move(s), .style=Style{},
                                    .wrap=TextWrap::NoWrap, .runs=std::move(r) }};
    }
};

} // namespace maya
