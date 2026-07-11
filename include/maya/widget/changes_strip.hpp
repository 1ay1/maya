#pragma once
// maya::widget::ChangesStrip — bordered "session has pending changes" banner.
//
// Header row + file list inside a rounded border. The header carries the
// "Changes (N files)" label and key hints for review / accept / reject.
// The body is a maya::FileChanges list. When `changes` is empty the
// widget renders to an empty Element so callers can drop it into a
// vertical stack without a conditional.
//
//   maya::ChangesStrip{{
//       .changes      = file_change_vector,
//       .border_color = Color::yellow(),
//   }}.build();

#include <string>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../style/border.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "file_changes.hpp"

namespace maya {

class ChangesStrip {
public:
    struct Config {
        std::vector<FileChange> changes;

        // Brand palette
        Color border_color  = Color::yellow();
        Color text_color    = Color::bright_white();
        Color accept_color  = Color::green();
        Color reject_color  = Color::red();
    };

    explicit ChangesStrip(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        if (cfg_.changes.empty()) return text("");

        const Color muted = Color::bright_black();

        // Header is a fit_row: the "Changes (N files)" label and the grow
        // spacer are essential; the key hints shed lowest-keep first
        // (review → reject → accept) when the strip is too narrow —
        // measured, so a relabeled hint re-decides by itself.
        std::vector<FitItem> header;
        header.push_back({h(
            text("Changes ", Style{}.with_fg(cfg_.border_color).with_bold()),
            text("(" + std::to_string(cfg_.changes.size()) + " files)",
                 Style{}.with_fg(muted))).build()});
        header.push_back({Element{spacer().build()}});
        header.push_back({h(
            text("Ctrl+R", Style{}.with_fg(cfg_.text_color)),
            text(" review  ", Style{}.with_fg(muted))).build(), 1});
        header.push_back({h(
            text("A", Style{}.with_fg(cfg_.accept_color)),
            text(" accept  ", Style{}.with_fg(muted))).build(), 3});
        header.push_back({h(
            text("X", Style{}.with_fg(cfg_.reject_color)),
            text(" reject", Style{}.with_fg(muted))).build(), 2});

        FileChanges fc;
        for (const auto& c : cfg_.changes) fc.add(c);

        return (v(detail::fit_row(std::move(header)), fc.build())
                | border(BorderStyle::Round)
                | bcolor(cfg_.border_color)
                | padding(0, 1)
               ).build();
    }

private:
    Config cfg_;
};

} // namespace maya
