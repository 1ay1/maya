// markup.cpp — the markdown engine + the HTML widget, two ways.
//
//   ./maya_markup           interactive, scrollable viewer (default)
//   ./maya_markup --dump    one-shot colored dump to stdout (pipe / less -R)
//
// The document (four panels) is rendered ONCE per width into a canvas, then
// every canvas row is extracted as a plain styled TextElement. The viewer
// then slice-scrolls that flat line list (the scroll_slice.cpp pattern):
// only the visible rows are ever put in the Element tree, and they are plain
// text — no ComponentElements whose height a scroll viewport can't measure.
// That keeps scrolling correct and O(viewport), and re-flattening on resize
// keeps it responsive. A full flatten is ~1 ms.
//
// Four panels, each full width:
//   1. CommonMark + GFM: headings, emphasis, nested loose/tight lists, a
//      table, a blockquote, a fenced code block, links/autolinks.
//   2. Inline HTML in markdown: <b>/<i>/<kbd>/<mark>/<sub>/<sup>/<code>/<a>/
//      <br> become styled runs (markdown delegates tag semantics to maya::html).
//   3. A raw HTML *block* in markdown — parsed and rendered, not literal tags.
//   4. The standalone maya::html widget rendering a full HTML fragment.
//
// Keys: ↑/↓ scroll · PgUp/PgDn page · Home/End jump · q/Esc quit.

#include <maya/maya.hpp>
#include <maya/widget/html.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/scrollbar.hpp>
#include <maya/render/diff.hpp>  // detail::encode_utf8

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace maya;
using namespace maya::dsl;

namespace {

// ── content ────────────────────────────────────────────────────────────────

// A titled panel that stretches to the full available width.
Element panel(std::string_view title, Element body) {
    auto head = Element{TextElement{
        .content = std::string(title),
        .style = Style{}.with_bold().with_fg(Color::hex(0x7DCFFF)),
    }};
    return vstack()
        .align_self(Align::Stretch)
        .border(BorderStyle::Round)
        .border_color(Color::hex(0x3B4261))
        .padding(0, 1, 0, 1)
        .gap(1)(std::move(head), std::move(body));
}

constexpr std::string_view kMarkdown = R"(# maya markdown

Supports **bold**, *italic*, ***both***, `inline code`,
[links](https://example.com) and autolinks <https://maya.dev>.

Tight list:
- alpha
- beta
  - nested gamma
  - nested delta

Loose list (blank lines between items):

1. first paragraph

2. second paragraph

> Block quotes nest and stay tight,
> across soft-wrapped lines.

| Feature   | State |
|-----------|:-----:|
| CommonMark| 100%  |
| GFM tables| yes   |

```cpp
auto ui = maya::markdown(source);
```
)";

constexpr std::string_view kInlineHtml =
    "Inline HTML is interpreted: <b>bold</b>, <i>italic</i>, "
    "<u>underline</u>, <mark>highlight</mark>, <code>code()</code>, "
    "press <kbd>Ctrl</kbd>+<kbd>C</kbd>, H<sub>2</sub>O, e=mc<sup>2</sup>, "
    "a <a href=\"https://maya.dev\">styled link</a>.<br>"
    "A &lt;br&gt; above forced this new line &mdash; entities decode too.";

constexpr std::string_view kBlockHtml = R"(A raw HTML block in markdown:

<table>
  <tr><th>Lang</th><th>Speed</th></tr>
  <tr><td>C++</td><td>fast</td></tr>
  <tr><td>maya</td><td>faster</td></tr>
</table>

<details>
  <summary>Click to expand</summary>
  <p>Hidden content rendered inline, with a <b>bold</b> word.</p>
</details>
)";

constexpr std::string_view kHtmlDoc = R"(
<h1>maya::html</h1>
<p>A standalone widget: tokenizer &rarr; DOM &rarr; Element, built on the
   maya DSL. Whitespace
   collapses    like a browser.</p>
<h2>Phrasing</h2>
<p><b>bold</b>, <em>em</em>, <s>strike</s>, <code>code</code>,
   <kbd>Esc</kbd>, <a href="/x">link</a>.</p>
<ul>
  <li>unordered one</li>
  <li>unordered two
    <ol start="5">
      <li>nested five</li>
      <li>nested six</li>
    </ol>
  </li>
</ul>
<blockquote>Block quotes get a gutter bar.</blockquote>
<pre>preformatted
  whitespace   preserved</pre>
<hr>
<table>
  <thead><tr><th>Col A</th><th>Col B</th></tr></thead>
  <tbody>
    <tr><td>1</td><td>two</td></tr>
    <tr><td>three</td><td>4</td></tr>
  </tbody>
</table>
)";

// The html widget is fully themable; tweak a couple of slots for panel 4.
constexpr Theme accent = Theme::derive(theme::dark, [](Theme& t) {
    t.primary = Color::hex(0x9ECE6A);  // green h1
    t.link    = Color::hex(0xE0AF68);  // amber links
});

Element build_doc() {
    std::vector<Element> sections;
    sections.push_back(panel("1 · CommonMark + GFM", markdown(kMarkdown)));
    sections.push_back(panel("2 · Inline HTML in markdown", markdown(kInlineHtml)));
    sections.push_back(panel("3 · HTML block in markdown", markdown(kBlockHtml)));
    sections.push_back(panel("4 · Standalone html::render (custom theme)",
                             html::render(kHtmlDoc, accent)));
    return vstack().gap(1)(std::move(sections));
}

// ── render the document to a flat list of styled lines ──────────────────────
// Paint the tree into a canvas at `width`, then turn each row into a plain
// TextElement carrying the row's styled runs. The result has no
// ComponentElements, so it slices and scrolls perfectly.
std::vector<Element> flatten_to_lines(const Element& root, int width) {
    std::vector<Element> lines;
    if (width < 1) return lines;
    StylePool pool;
    Canvas canvas{width, 16000, &pool};
    render_tree(root, canvas, pool, theme::dark, /*auto_height=*/true);
    int rows = content_height(canvas);
    lines.reserve(static_cast<std::size_t>(rows) + 1);
    for (int y = 0; y <= rows; ++y) {
        int last = -1;  // rightmost non-blank column (trim trailing blanks)
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch != U' ' && ch != 0) last = x;
        }
        std::string content;
        std::vector<StyledRun> runs;
        std::uint16_t cur = 0;
        std::size_t run_start = 0;
        bool open = false;
        for (int x = 0; x <= last; ++x) {
            Cell cell = canvas.get(x, y);
            if (cell.character == 0) continue;  // wide-char continuation cell
            if (!open || cell.style_id != cur) {
                if (open && content.size() > run_start)
                    runs.push_back({run_start, content.size() - run_start,
                                    pool.get(cur)});
                cur = cell.style_id;
                run_start = content.size();
                open = true;
            }
            detail::encode_utf8(cell.character, content);
        }
        if (open && content.size() > run_start)
            runs.push_back({run_start, content.size() - run_start, pool.get(cur)});
        lines.push_back(Element{TextElement{
            .content = std::move(content),
            .style = Style{},
            .wrap = TextWrap::NoWrap,  // already laid out — never re-wrap
            .runs = std::move(runs),
        }});
    }
    return lines;
}

// Terminal width for the one-shot dump (full columns, or 80 when not a TTY).
int dump_width() {
#if !defined(_WIN32)
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return std::max<int>(ws.ws_col, 40);
#endif
    return 80;
}

void dump() {
    int width = dump_width();
    StylePool pool;
    Canvas canvas{width, 16000, &pool};
    render_tree(build_doc(), canvas, pool, theme::dark, /*auto_height=*/true);
    int rows = content_height(canvas);
    if (rows < 0) return;
    std::string out;
    serialize(canvas, pool, out, rows + 1, 0);
    out += "\x1b[?7h";  // serialize disables autowrap; restore it
    out += '\n';
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }

    const Element doc = build_doc();
    std::vector<Element> lines;   // flattened doc, rebuilt on width change
    int flat_w = -1;              // width `lines` was flattened at
    int offset = 0;               // top visible line
    int viewport_h = 1;

    auto max_offset = [&] {
        return std::max(0, static_cast<int>(lines.size()) - viewport_h);
    };

    run({.title = "markup"},
        [&](const Event& ev) {
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return false;
            int step = std::max(1, viewport_h - 1);
            if (key(ev, SpecialKey::Up))       offset -= 1;
            if (key(ev, SpecialKey::Down))     offset += 1;
            if (key(ev, SpecialKey::PageUp))   offset -= step;
            if (key(ev, SpecialKey::PageDown)) offset += step;
            if (key(ev, SpecialKey::Home))     offset = 0;
            if (key(ev, SpecialKey::End))      offset = max_offset();
            if (auto* m = as_mouse(ev); m && m->kind == MouseEventKind::Press) {
                if (m->button == MouseButton::ScrollUp)   offset -= 3;
                if (m->button == MouseButton::ScrollDown) offset += 3;
            }
            offset = std::clamp(offset, 0, max_offset());
            return true;
        },
        [&](const Ctx& ctx) -> Element {
            const int W = ctx.size.width.value;
            const int H = ctx.size.height.value;
            // Flatten to the content width, reserving 1 column for the
            // scrollbar. Re-flatten only when the width changes.
            const int content_w = std::max(1, W - 1);
            if (content_w != flat_w) {
                lines = flatten_to_lines(doc, content_w);
                flat_w = content_w;
            }
            viewport_h = std::max(1, H - 3);  // header + help + status rows
            offset = std::clamp(offset, 0, max_offset());

            // Emit ONLY the visible window — the slice pattern.
            const int start = std::clamp(offset, 0, max_offset());
            const int count = std::min(
                viewport_h, static_cast<int>(lines.size()) - start);
            std::vector<Element> visible(
                lines.begin() + start, lines.begin() + start + std::max(0, count));

            // Mirror the manual offset into a ScrollState so scrollbar_y can
            // draw the thumb. auto_dispatch = false: we drive scrolling in the
            // event handler, so the runtime must not also move this state.
            ScrollState bar;
            bar.y = start;
            bar.max_y = max_offset();
            bar.auto_dispatch = false;

            const int total = static_cast<int>(lines.size());
            std::string status =
                "lines " + std::to_string(start + 1) + "–" +
                std::to_string(start + std::max(0, count)) + " / " +
                std::to_string(total);

            return v(
                t<"maya markup — CommonMark + GFM + HTML widget">
                    | Bold | Fg<125, 207, 255>,
                t<"↑/↓ PgUp/PgDn Home/End scroll · q quit"> | Dim,
                h(
                    v(std::move(visible)) | grow_<1>,
                    scrollbar_y(bar, viewport_h)
                ),
                text(status) | Fg<224, 175, 104>
            );
        });
    return 0;
}
