// scroll_slice.cpp — Pattern 2: slice-based scrolling (no clip, no overflow).
//
// The opposite trade-off from scroll_clip.cpp. Instead of painting all rows
// and letting the renderer drop the ones outside the viewport, we only emit
// the currently-visible rows into the Element tree. Nothing off-screen is
// ever constructed, laid out, or painted.
//
// This is how maya's list widgets (log_viewer, select, list, textarea) work
// internally — see include/maya/widget/log_viewer.hpp:201 (build()) for the
// canonical version with filtering, auto-scroll-to-tail, and focus support.
//
// Trade-offs vs. clip-based:
//   + cheaper: no overdraw, layout tree stays proportional to viewport
//   + works for million-row data sets
//   − requires the data to be indexable (a vector, not an opaque Element)
//   − caller hand-maintains scroll_offset and key/mouse routing
//
// Keys: ↑/↓ scroll · PgUp/PgDn page · Home/End jump · q quit.

#include <maya/host/run.hpp>
#include <maya/maya.hpp>

#include <algorithm>
#include <string>
#include <variant>
#include <optional>
#include <vector>

using namespace maya;
using namespace maya::dsl;

struct ScrollList {
    std::vector<std::string> items;
    int viewport_h = 8;
    int offset     = 0;

    [[nodiscard]] int max_offset() const {
        return std::max(0, static_cast<int>(items.size()) - viewport_h);
    }
    void scroll_by(int delta) {
        offset = std::clamp(offset + delta, 0, max_offset());
    }
    void to_top()    { offset = 0; }
    void to_bottom() { offset = max_offset(); }

    [[nodiscard]] Element build() const {
        // Emit ONLY the visible window — this is the load-bearing line.
        const int start = std::clamp(offset, 0, max_offset());
        const int count = std::min(viewport_h, static_cast<int>(items.size()) - start);

        std::vector<Element> rows;
        rows.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const int abs_idx = start + i;
            auto row = text(items[static_cast<std::size_t>(abs_idx)]);
            if (abs_idx % 2 == 0) row = row | Dim;
            rows.push_back(row);
        }
        return v(std::move(rows));
    }
};

ScrollList make_list() {
    ScrollList list;
    list.viewport_h = 8;
    for (int i = 1; i <= 200; ++i)
        list.items.push_back("row " + std::to_string(i) + " — slice-pattern rendering");
    return list;
}

struct Model { ScrollList list = make_list(); };

struct ScrollBy { int delta; };   // rows; kPage = one viewport
struct Top {};
struct Bottom {};
struct Quit {};
using Msg = std::variant<ScrollBy, Top, Bottom, Quit>;
constexpr int kPage = 1 << 20;

struct ScrollSlice {
    using Model = ::Model;
    using Msg   = ::Msg;
    using Cmd   = jaal::Cmd<Msg>;
    using Sub   = jaal::Sub<Msg, on_key, on_mouse>;

    static Cmd update(Model& m, ScrollBy s) {
        const int page = m.list.viewport_h;
        m.list.scroll_by(s.delta == kPage ? page : s.delta == -kPage ? -page : s.delta);
        return {};
    }
    static Cmd update(Model& m, Top)    { m.list.to_top(); return {}; }
    static Cmd update(Model& m, Bottom) { m.list.to_bottom(); return {}; }
    static Cmd update(Model&, Quit)     { return Cmd::quit(0); }

    static Element view(const Model& m) {
        const auto& list = m.list;
        const int total = static_cast<int>(list.items.size());
        const int shown_to = std::min(list.offset + list.viewport_h, total);
        const std::string status = "rows " + std::to_string(list.offset + 1) + "–" +
                                   std::to_string(shown_to) + " of " + std::to_string(total);
        return v(
            t<"Slice-based pattern — emit only the visible window"> | Bold | Fg<100, 180, 255>,
            t<"Off-screen rows are never laid out or painted."> | Dim,
            blank_,
            list.build(),
            blank_,
            text(status) | Dim,
            t<"↑/↓ j/k row · PgUp/PgDn page · Home/End · q quit"> | Dim
        ) | pad<1> | border_<Round> | bcol<50, 55, 70>;
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(
            keys<Sub>({
                {'q', Quit{}}, {'k', ScrollBy{-1}}, {'j', ScrollBy{+1}}, {SpecialKey::Up, ScrollBy{-1}}, {SpecialKey::Down, ScrollBy{+1}},
                {SpecialKey::PageUp, ScrollBy{-kPage}}, {SpecialKey::PageDown, ScrollBy{+kPage}},
                {SpecialKey::Home, Top{}}, {SpecialKey::End, Bottom{}},
            }),
            Sub::on(on_mouse{}, [](const MouseEvent& e) -> std::optional<Msg> {
                if (e.kind != MouseEventKind::Press) return std::nullopt;
                if (e.button == MouseButton::ScrollUp)   return ScrollBy{-1};
                if (e.button == MouseButton::ScrollDown) return ScrollBy{+1};
                return std::nullopt;
            }));
    }
    static bool subs_key(const Model&) { return true; }
};

static_assert(Program<ScrollSlice>);

int main() { return run<ScrollSlice>({.title = "scroll_slice", .mouse = true}); }
