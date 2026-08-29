#pragma once
// maya::widget::FileTree — file explorer sidebar (background-free)
//
// A genuinely nice terminal file tree, in the spirit of nvim-tree / neo-tree /
// VSCode's explorer:
//
//   • real tree connectors (├─ └─ │) with correct last-child detection
//   • open/closed folder glyphs + filename- AND extension-aware file icons
//     (Dockerfile, Makefile, package.json, .gitignore, LICENSE, …)
//   • a right-aligned status column so names stay aligned: diagnostic count
//     badges (● N) and a git letter (M/A/U/D), each in its own colour
//   • symlinks shown as “name → target”, hidden/ignored entries dimmed+italic
//   • long names truncated with … while the status column stays visible
//   • the active row gets a soft full-width shade and a left accent bar
//
// You feed it a flat, already-expanded list of rows (name, depth, kind); the
// widget derives the connector shape from the depth sequence. Per-row extras
// are set fluently on the row you just added:
//
//   FileTree t;
//   t.root("rope");
//   t.folder("src", 0);
//     t.file("rope.cpp", 1).git(GitState::Modified).diag(2, 0);
//     t.file("rope.hpp", 1);
//   t.folder("tests", 0, /*open=*/false).count(4);
//   t.file(".gitignore", 0).hidden();
//   t.file("link.hpp", 0).symlink("../real/link.hpp");
//   t.active(1);
//   Element ui = t | dsl::width(32);

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../dsl.hpp"
#include "../element/text.hpp"
#include "../style/color.hpp"
#include "../style/style.hpp"
#include "fuzzy_line.hpp"

namespace maya {

enum class GitState : uint8_t { None, Modified, Added, Untracked, Deleted, Ignored };

struct FileTreeTheme {
    Color folder    = Color::hex(0x89B4FA); // folder name
    Color file      = Color::hex(0xBAC2DE); // file name
    Color guide     = Color::hex(0x45475A); // tree connectors
    Color accent    = Color::hex(0x89B4FA); // active-row left bar
    Color active    = Color::hex(0x232634); // active-row wash
    Color hidden    = Color::hex(0x585B70); // dotfiles / ignored
    Color link      = Color::hex(0x94E2D5); // symlink arrow + target
    Color count     = Color::hex(0x585B70); // collapsed child count
    Color match     = Color::hex(0xF9E2AF); // filter match highlight
    Color mark      = Color::hex(0xCBA6F7); // marked-file indicator
    Color err       = Color::hex(0xF38BA8); // ● error badge
    Color warn      = Color::hex(0xE2B341); // ● warning badge
    Color modified  = Color::hex(0xE2B341); // M
    Color added     = Color::hex(0xA6E3A1); // A
    Color untracked = Color::hex(0x94E2D5); // U
    Color deleted   = Color::hex(0xF38BA8); // D
};

class FileTree {
public:
    // How to shorten a name that doesn't fit. KeepExtension is best for files:
    // it trims the stem but always keeps ".ext" (so the type stays readable).
    enum class Trunc { KeepExtension, Middle, End };

    FileTree& root(std::string name) { root_ = std::move(name); return *this; }

    FileTree& folder(std::string name, int depth, bool open = true) {
        rows_.push_back({std::move(name), depth, true, open, {}});
        return *this;
    }
    FileTree& file(std::string name, int depth, GitState git = GitState::None) {
        Row r{std::move(name), depth, false, false, {}};
        r.git = git;
        rows_.push_back(std::move(r));
        return *this;
    }

    // Per-row extras — call right after folder()/file().
    FileTree& git(GitState g)         { last().git = g; return *this; }
    FileTree& diag(int errors, int warnings) { last().err = errors; last().warn = warnings; return *this; }
    FileTree& hidden(bool v = true)   { last().hidden = v; return *this; }
    FileTree& symlink(std::string t)  { last().link = std::move(t); return *this; }
    FileTree& count(int n)            { last().child_count = n; return *this; }
    FileTree& marked(bool v = true)   { last().marked = v; return *this; }
    FileTree& active(int i)           { active_ = i; return *this; }

    // Override the color theme.
    FileTree& set_theme(FileTreeTheme t) { theme = std::move(t); return *this; }

    // Live filter: highlight the query's characters within each name.
    FileTree& filter(std::string q)   { filter_ = std::move(q); return *this; }

    // Truncation strategy (default KeepExtension) and whether the focused row
    // reveals its full name by dropping the status column.
    FileTree& truncate(Trunc t)       { trunc_ = t; return *this; }
    FileTree& reveal_active(bool v = true) { reveal_ = v; return *this; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        const int n = static_cast<int>(rows_.size());

        // last-child detection: row i is the last child of its parent if the
        // next row with depth <= depth[i] is shallower (or there is none).
        std::vector<bool> is_last(static_cast<size_t>(n), true);
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                if (rows_[j].depth < rows_[i].depth) break;
                if (rows_[j].depth == rows_[i].depth) { is_last[i] = false; break; }
            }
        }

        std::vector<Element> out;
        out.reserve(static_cast<size_t>(n) + 1);

        if (!root_.empty()) {
            std::string s = "\xef\x84\xbc  " + root_; //  open folder
            std::vector<StyledRun> r{
                {0, 6, Style{}.with_fg(theme.folder)},
                {6, root_.size(), Style{}.with_fg(theme.folder).with_bold()}};
            out.push_back(Element{TextElement{ .content = std::move(s), .style = Style{},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(r) }});
        }

        std::vector<bool> cont; // cont[level] => ancestor at that level has more siblings
        for (int i = 0; i < n; ++i) {
            const Row& e = rows_[static_cast<size_t>(i)];
            const int d = e.depth;
            if (static_cast<int>(cont.size()) <= d) cont.resize(static_cast<size_t>(d) + 1, false);

            // connector prefix
            std::string guides;
            for (int a = 1; a < d; ++a) guides += cont[static_cast<size_t>(a)] ? "\xe2\x94\x82 " : "  ";
            if (d >= 1) guides += is_last[static_cast<size_t>(i)] ? "\xe2\x94\x94\xe2\x94\x80 " // └─
                                                                  : "\xe2\x94\x9c\xe2\x94\x80 "; // ├─
            cont[static_cast<size_t>(d)] = !is_last[static_cast<size_t>(i)];

            out.push_back(row(e, std::move(guides), i == active_));
        }
        return dsl::v(std::move(out)).build();
    }

private:
    struct Row {
        std::string name;
        int         depth = 0;
        bool        dir   = false;
        bool        open  = false;
        std::string link;
        GitState    git   = GitState::None;
        bool        hidden = false;
        bool        marked = false;
        int         err = 0, warn = 0;
        int         child_count = -1;
    };

    std::vector<Row> rows_;
    std::string      root_;
    std::string      filter_;
    int              active_ = -1;
    Trunc            trunc_  = Trunc::KeepExtension;
    bool             reveal_ = true;
    FileTreeTheme    theme;

    Row& last() { return rows_.back(); }

    // Push a name, highlighting the filter query's matched characters.
    template <class Tint>
    static void push_name(std::string& s, std::vector<StyledRun>& runs,
                          const std::string& shown, Style base, Tint&& tint,
                          const std::string& filter, Color match_col) {
        if (filter.empty()) {
            runs.push_back({s.size(), shown.size(), tint(base)});
            s += shown;
            return;
        }
        auto hits = FuzzyLine::match(shown, filter);
        const Style hit = base.with_fg(match_col).with_bold();
        size_t mi = 0;
        for (size_t i = 0; i < shown.size(); ++i) {
            bool m = (mi < hits.positions.size() && static_cast<int>(i) == hits.positions[mi]);
            if (m) ++mi;
            runs.push_back({s.size(), 1, tint(m ? hit : base)});
            s += shown[i];
        }
    }

    // Shorten `name` to fit `avail` columns per the given strategy.
    static std::string shorten(const std::string& name, int avail, Trunc trunc) {
        if (string_width(name) <= avail) return name;
        if (avail <= 1) return "\xe2\x80\xa6"; // …
        switch (trunc) {
            case Trunc::End:    return maya::detail::truncate_end(name, avail);
            case Trunc::Middle: return maya::detail::truncate_middle(name, avail);
            case Trunc::KeepExtension: {
                auto dot = name.find_last_of('.');
                if (dot == std::string::npos || dot == 0)
                    return maya::detail::truncate_middle(name, avail);
                std::string ext = name.substr(dot);          // ".tsx"
                int extw = string_width(ext);
                if (extw >= avail - 1)                        // ext alone too long
                    return maya::detail::truncate_middle(name, avail);
                std::string stem = maya::detail::truncate_end(name.substr(0, dot),
                                                              avail - extw);
                return stem + ext;
            }
        }
        return maya::detail::truncate_end(name, avail);
    }

    Element row(const Row& e, std::string guides, bool on) const {
        const Color shade = on ? theme.active : Color{};
        auto tint = [on, shade](Style st) { return on ? st.with_bg(shade) : st; };

        // ── left segment: accent bar + guides + icon + name (+ link) ────────
        std::string L; std::vector<StyledRun> lr;
        auto put = [&](std::string& s, std::vector<StyledRun>& runs,
                       std::string_view t, Style st) {
            if (t.empty()) return; runs.push_back({s.size(), t.size(), tint(st)}); s += t;
        };
        // leading indicator: marked ┃ (accent) > active ▎ (accent) > space
        put(L, lr, e.marked ? "\xe2\x94\x83" : (on ? "\xe2\x96\x8e" : " "),
            Style{}.with_fg(e.marked ? theme.mark : theme.accent));
        put(L, lr, guides, Style{}.with_fg(theme.guide));

        const bool dim = e.hidden || e.git == GitState::Ignored;
        if (e.dir) {
            put(L, lr, e.open ? "\xef\x84\xbc " : "\xef\x84\xbb ",  //  / 
                Style{}.with_fg(dim ? theme.hidden : theme.folder));
        } else {
            put(L, lr, std::string(glyph(e.name)) + " ",
                Style{}.with_fg(dim ? theme.hidden : icon_color(e.name)));
        }

        // name + optional collapsed child count / symlink target
        const int prefix_w = string_width(L);
        std::string name = e.name;
        std::string tail; // link/count, rendered dim after the name

        // ── right segment: diagnostics + git ───────────────────────────────
        std::string R; std::vector<StyledRun> rr;
        if (e.err > 0)  put(R, rr, "\xe2\x97\x8f" + std::to_string(e.err) + " ", Style{}.with_fg(theme.err));
        else if (e.warn > 0) put(R, rr, "\xe2\x97\x8f" + std::to_string(e.warn) + " ", Style{}.with_fg(theme.warn));
        if (e.git != GitState::None && e.git != GitState::Ignored)
            put(R, rr, git_letter(e.git), Style{}.with_fg(git_color(e.git)));

        Style name_st = e.dir ? Style{}.with_fg(dim ? theme.hidden : theme.folder).with_bold()
                              : Style{}.with_fg(dim ? theme.hidden : theme.file);
        if (dim) name_st = name_st.with_italic();

        // Capture everything the deferred paint needs BY VALUE — the FileTree
        // itself may be a temporary, so the render lambda must not touch `this`.
        const std::string filt = filter_;
        const Trunc  tr    = trunc_;
        const bool   rev   = reveal_;
        const Color  cmatch = theme.match, clink = theme.link, ccount = theme.count;

        return Element{ComponentElement{
            .render = [L, lr, R, rr, name, name_st, prefix_w, link = e.link,
                       dir = e.dir, open = e.open, ccnt = e.child_count,
                       on, shade, tint, filt, tr, rev, cmatch, clink, ccount]
                      (int w, int) -> Element {
                std::string s = L; std::vector<StyledRun> runs = lr;
                const Style fill = on ? Style{}.with_bg(shade) : Style{};

                const bool reveal = on && rev && !R.empty();
                const int right_w = reveal ? 0 : string_width(R);
                int avail = w - prefix_w - right_w - 1;
                if (avail < 3) avail = std::max(1, w - prefix_w);

                if (link.empty()) {
                    push_name(s, runs, shorten(name, avail, tr), name_st, tint, filt, cmatch);
                } else if (string_width(name) + 4 <= avail) {
                    push_name(s, runs, name, name_st, tint, filt, cmatch);
                    runs.push_back({s.size(), 5, tint(Style{}.with_fg(clink))});
                    s += " \xe2\x86\x92 "; // →
                    std::string tgt = maya::detail::truncate_middle(
                        link, std::max(1, avail - string_width(name) - 3));
                    runs.push_back({s.size(), tgt.size(),
                                    tint(Style{}.with_fg(clink).with_italic())});
                    s += tgt;
                } else {
                    push_name(s, runs, shorten(name, avail, tr), name_st, tint, filt, cmatch);
                }

                if (dir && !open && ccnt >= 0) {
                    std::string c = " " + std::to_string(ccnt);
                    runs.push_back({s.size(), c.size(), tint(Style{}.with_fg(ccount))});
                    s += c;
                }

                if (!reveal && !R.empty()) {
                    int used = string_width(s) + right_w;
                    int gap = std::max(1, w - used);
                    runs.push_back({s.size(), static_cast<size_t>(gap), fill});
                    s.append(static_cast<size_t>(gap), ' ');
                    size_t roff = s.size();
                    s += R;
                    for (auto run : rr)
                        runs.push_back({roff + run.byte_offset, run.byte_length, run.style});
                }

                int total = string_width(s);
                if (total < w) {
                    runs.push_back({s.size(), static_cast<size_t>(w - total), fill});
                    s.append(static_cast<size_t>(w - total), ' ');
                }
                return Element{TextElement{ .content = std::move(s), .style = fill,
                                            .wrap = TextWrap::NoWrap, .runs = std::move(runs) }};
            },
            .measure = [](int mw) -> Size { return Size{Columns(mw), Rows(1)}; },
        }};
    }

    const char* git_letter(GitState g) const {
        switch (g) {
            case GitState::Modified:  return "M";
            case GitState::Added:     return "A";
            case GitState::Untracked: return "U";
            case GitState::Deleted:   return "D";
            default:                  return " ";
        }
    }
    Color git_color(GitState g) const {
        switch (g) {
            case GitState::Modified:  return theme.modified;
            case GitState::Added:     return theme.added;
            case GitState::Untracked: return theme.untracked;
            case GitState::Deleted:   return theme.deleted;
            default:                  return theme.file;
        }
    }

    // Filename-aware, then extension-aware icon.
    static const char* glyph(std::string_view n) {
        auto is = [&](std::string_view x) { return n == x; };
        auto ends = [&](std::string_view e) {
            return n.size() >= e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0;
        };
        if (is("Dockerfile") || is(".dockerignore")) return "\xef\x8c\x88"; // 
        if (is("Makefile") || is("CMakeLists.txt"))  return "\xee\x9f\xa8"; // 
        if (is("package.json") || is("package-lock.json")) return "\xee\x98\x8b"; // 
        if (is(".gitignore") || is(".gitattributes")) return "\xef\x87\x93"; // 
        if (is("LICENSE") || is("LICENSE.md"))        return "\xef\x9c\xb6"; // 
        if (is(".env") || ends(".env"))               return "\xef\x93\xa2"; // 
        if (ends(".cpp") || ends(".cc") || ends(".cxx")) return "\xee\x98\x9d"; // 
        if (ends(".hpp") || ends(".h") || ends(".hxx"))  return "\xef\x83\x93"; // 
        if (ends(".rs"))   return "\xee\x9e\xa8"; // 
        if (ends(".py"))   return "\xee\x98\x86"; // 
        if (ends(".go"))   return "\xee\x98\xa7"; // 
        if (ends(".ts"))   return "\xee\x98\xa8"; // 
        if (ends(".js"))   return "\xee\x9e\x8e"; // 
        if (ends(".json")) return "\xee\x98\x8b"; // 
        if (ends(".md"))   return "\xef\x92\x89"; // 
        if (ends(".toml") || ends(".yaml") || ends(".yml")) return "\xee\x9a\x95"; // 
        if (ends(".lock"))  return "\xef\x8a\x9c"; // 
        return "\xef\x85\x9b"; // 
    }
    static Color icon_color(std::string_view n) {
        auto ends = [&](std::string_view e) {
            return n.size() >= e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0;
        };
        if (ends(".cpp") || ends(".cc") || ends(".cxx") ||
            ends(".hpp") || ends(".h")) return Color::hex(0x649AD2);
        if (ends(".rs"))   return Color::hex(0xDEA584);
        if (ends(".py"))   return Color::hex(0xFFD43B);
        if (ends(".go"))   return Color::hex(0x00ADD8);
        if (ends(".ts"))   return Color::hex(0x3178C6);
        if (ends(".js"))   return Color::hex(0xF7DF1E);
        if (ends(".json")) return Color::hex(0xCBCB41);
        if (ends(".md"))   return Color::hex(0x89B4FA);
        if (ends(".toml") || ends(".yaml") || ends(".yml")) return Color::hex(0xCB9B6A);
        return Color::hex(0x9399B2);
    }
};

} // namespace maya
