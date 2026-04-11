#pragma once
// maya::widget::git_status — Git branch and working tree status
//
// Displays the current branch, ahead/behind counts, and dirty state.
// Compact format for status bars, or expanded with file change list.
//
//   GitStatusWidget gs;
//   gs.set_branch("main");
//   gs.set_ahead(2);
//   gs.set_dirty(3, 1, 5);  // modified, added, untracked
//   auto ui = gs.build();

#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class GitStatusWidget {
    std::string branch_;
    int  ahead_     = 0;
    int  behind_    = 0;
    int  modified_  = 0;
    int  staged_    = 0;
    int  untracked_ = 0;
    int  deleted_   = 0;
    int  conflicts_ = 0;
    bool compact_   = true;
    std::vector<std::string> changed_files_;

public:
    GitStatusWidget() = default;

    void set_branch(std::string b) { branch_ = std::move(b); }
    void set_ahead(int n) { ahead_ = n; }
    void set_behind(int n) { behind_ = n; }
    void set_dirty(int mod, int staged, int untracked) {
        modified_ = mod; staged_ = staged; untracked_ = untracked;
    }
    void set_deleted(int n) { deleted_ = n; }
    void set_conflicts(int n) { conflicts_ = n; }
    void set_compact(bool b) { compact_ = b; }
    void add_changed_file(std::string f) { changed_files_.push_back(std::move(f)); }

    [[nodiscard]] bool is_clean() const {
        return modified_ == 0 && staged_ == 0 && untracked_ == 0
            && deleted_ == 0 && conflicts_ == 0;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto lbl   = Style{}.with_fg(Color::rgb(127, 132, 142));
        auto val   = Style{}.with_fg(Color::rgb(200, 204, 212));

        Color branch_color = is_clean()
            ? Color::rgb(152, 195, 121)   // green = clean
            : Color::rgb(229, 192, 123);  // yellow = dirty

        // ── Compact: single line for status bar ──────────────────────
        if (compact_) {
            std::vector<Element> parts;

            // Branch icon + name
            parts.push_back(text("\xee\x82\xa0 ", Style{}.with_fg(branch_color))); //
            parts.push_back(text(branch_.empty() ? "detached" : branch_,
                                 Style{}.with_fg(branch_color).with_bold()));

            // Ahead/behind
            if (ahead_ > 0) {
                parts.push_back(text(" \xe2\x86\x91" + std::to_string(ahead_), // ↑
                    Style{}.with_fg(Color::rgb(152, 195, 121))));
            }
            if (behind_ > 0) {
                parts.push_back(text(" \xe2\x86\x93" + std::to_string(behind_), // ↓
                    Style{}.with_fg(Color::rgb(224, 108, 117))));
            }

            // Dirty indicators
            if (!is_clean()) {
                std::string dirty;
                if (modified_ > 0)  dirty += " ~" + std::to_string(modified_);
                if (staged_ > 0)    dirty += " +" + std::to_string(staged_);
                if (deleted_ > 0)   dirty += " -" + std::to_string(deleted_);
                if (untracked_ > 0) dirty += " ?" + std::to_string(untracked_);
                if (conflicts_ > 0) dirty += " !" + std::to_string(conflicts_);
                parts.push_back(text(dirty,
                    Style{}.with_fg(Color::rgb(229, 192, 123))));
            }

            return h(std::move(parts)).build();
        }

        // ── Expanded: multi-line with file list ──────────────────────
        std::vector<Element> rows;

        // Branch header
        rows.push_back(h(
            text("\xee\x82\xa0 ", Style{}.with_fg(branch_color)), //
            text(branch_.empty() ? "detached HEAD" : branch_,
                 Style{}.with_fg(branch_color).with_bold())
        ).build());

        // Ahead/behind
        if (ahead_ > 0 || behind_ > 0) {
            std::vector<Element> ab;
            ab.push_back(text("  "));
            if (ahead_ > 0)
                ab.push_back(text("\xe2\x86\x91" + std::to_string(ahead_) + " ahead ",
                    Style{}.with_fg(Color::rgb(152, 195, 121))));
            if (behind_ > 0)
                ab.push_back(text("\xe2\x86\x93" + std::to_string(behind_) + " behind",
                    Style{}.with_fg(Color::rgb(224, 108, 117))));
            rows.push_back(h(std::move(ab)).build());
        }

        // File status summary
        if (!is_clean()) {
            if (staged_ > 0)
                rows.push_back(h(
                    text("  ", lbl),
                    text(std::to_string(staged_) + " staged",
                         Style{}.with_fg(Color::rgb(152, 195, 121)))
                ).build());
            if (modified_ > 0)
                rows.push_back(h(
                    text("  ", lbl),
                    text(std::to_string(modified_) + " modified",
                         Style{}.with_fg(Color::rgb(229, 192, 123)))
                ).build());
            if (deleted_ > 0)
                rows.push_back(h(
                    text("  ", lbl),
                    text(std::to_string(deleted_) + " deleted",
                         Style{}.with_fg(Color::rgb(224, 108, 117)))
                ).build());
            if (untracked_ > 0)
                rows.push_back(h(
                    text("  ", lbl),
                    text(std::to_string(untracked_) + " untracked",
                         Style{}.with_fg(Color::rgb(127, 132, 142)))
                ).build());
            if (conflicts_ > 0)
                rows.push_back(h(
                    text("  ", lbl),
                    text(std::to_string(conflicts_) + " conflicts",
                         Style{}.with_fg(Color::rgb(224, 108, 117)).with_bold())
                ).build());
        } else {
            rows.push_back(text("  Working tree clean",
                Style{}.with_fg(Color::rgb(152, 195, 121)).with_dim()));
        }

        // Changed files list
        if (!changed_files_.empty()) {
            rows.push_back(text(""));
            for (auto& f : changed_files_) {
                rows.push_back(h(
                    text("    ", lbl),
                    text(f, val)
                ).build());
            }
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
