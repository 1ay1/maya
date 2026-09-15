// Tests for maya style system: Style attributes, SGR generation, merge, operator|
#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>   // markdown_palette_from
#include <maya/style/schemes.hpp>       // theme::schemes
// NDEBUG guard: CMake builds tests in Release (-O3 -DNDEBUG), which strips
// assert(). Undefine it here so this file's runtime asserts actually fire.
#undef NDEBUG
#include "agtest.hpp"
#include <print>

using namespace maya;

TEST_CASE("style empty sgr") {
    std::println("--- test_style_empty_sgr ---");
    assert(Style{}.to_sgr().empty());
    std::println("PASS\n");
}

TEST_CASE("style bold sgr") {
    std::println("--- test_style_bold_sgr ---");
    auto s = Style{}.with_bold();
    auto sgr = s.to_sgr();
    // Bold = SGR param "1", wrapped in ESC[...m
    assert(sgr.find("1") != std::string::npos);
    assert(s.bold == true);
    std::println("  bold sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style dim sgr") {
    std::println("--- test_style_dim_sgr ---");
    auto s = Style{}.with_dim();
    assert(s.dim == true);
    assert(s.to_sgr().find("2") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style italic sgr") {
    std::println("--- test_style_italic_sgr ---");
    auto s = Style{}.with_italic();
    assert(s.italic == true);
    assert(s.to_sgr().find("3") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style underline sgr") {
    std::println("--- test_style_underline_sgr ---");
    auto s = Style{}.with_underline();
    assert(s.underline == true);
    assert(s.to_sgr().find("4") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style inverse sgr") {
    std::println("--- test_style_inverse_sgr ---");
    auto s = Style{}.with_inverse();
    assert(s.inverse == true);
    assert(s.to_sgr().find("7") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style strikethrough sgr") {
    std::println("--- test_style_strikethrough_sgr ---");
    auto s = Style{}.with_strikethrough();
    assert(s.strikethrough == true);
    assert(s.to_sgr().find("9") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style fg color sgr") {
    std::println("--- test_style_fg_color_sgr ---");
    auto s = Style{}.with_fg(Color::green());
    assert(s.fg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("32") != std::string::npos); // green fg = 32
    std::println("  fg(green) sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style bg color sgr") {
    std::println("--- test_style_bg_color_sgr ---");
    auto s = Style{}.with_bg(Color::blue());
    assert(s.bg.has_value());
    auto sgr = s.to_sgr();
    assert(sgr.find("44") != std::string::npos); // blue bg = 44
    std::println("  bg(blue) sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style rgb fg sgr") {
    std::println("--- test_style_rgb_fg_sgr ---");
    auto s = Style{}.with_fg(Color::rgb(100, 150, 200));
    auto sgr = s.to_sgr();
    assert(sgr.find("38;2;100;150;200") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style combined sgr") {
    std::println("--- test_style_combined_sgr ---");
    auto s = Style{}.with_bold().with_fg(Color::red());
    auto sgr = s.to_sgr();
    assert(sgr.find("1")  != std::string::npos); // bold
    assert(sgr.find("31") != std::string::npos); // red fg
    std::println("  bold+red sgr: '{}'", sgr);
    std::println("PASS\n");
}

TEST_CASE("style merge additive") {
    std::println("--- test_style_merge_additive ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_italic().with_fg(Color::cyan());
    Style merged = a.merge(b);
    auto sgr = merged.to_sgr();
    assert(merged.bold == true);
    assert(merged.italic == true);
    assert(merged.fg.has_value());
    assert(sgr.find("36") != std::string::npos); // cyan fg
    std::println("PASS\n");
}

TEST_CASE("style merge second wins fg") {
    std::println("--- test_style_merge_second_wins_fg ---");
    Style a = Style{}.with_fg(Color::red());
    Style b = Style{}.with_fg(Color::blue());
    Style merged = a.merge(b);
    // Second wins for fg color
    assert(merged.fg.has_value());
    assert(merged.fg->fg_sgr() == "34"); // blue
    std::println("PASS\n");
}

TEST_CASE("style operator pipe") {
    std::println("--- test_style_operator_pipe ---");
    Style a = Style{}.with_bold();
    Style b = Style{}.with_fg(Color::magenta());
    Style combined = a | b;
    assert(combined.bold == true);
    assert(combined.fg.has_value());
    assert(combined.fg->fg_sgr() == "35"); // magenta
    std::println("PASS\n");
}

TEST_CASE("style pipe equals merge") {
    std::println("--- test_style_pipe_equals_merge ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_italic().with_bg(Color::blue());
    assert((a | b).to_sgr() == a.merge(b).to_sgr());
    std::println("PASS\n");
}

TEST_CASE("style empty predicate") {
    std::println("--- test_style_empty_predicate ---");
    assert( Style{}.empty());
    assert(!Style{}.with_bold().empty());
    assert(!Style{}.with_fg(Color::red()).empty());
    assert(!Style{}.with_bg(Color::red()).empty());
    std::println("PASS\n");
}

TEST_CASE("style predefined bold") {
    std::println("--- test_style_predefined_bold ---");
    assert(bold_style.bold == true);
    assert(bold_style.to_sgr().find("1") != std::string::npos);
    std::println("PASS\n");
}

TEST_CASE("style predefined dim") {
    std::println("--- test_style_predefined_dim ---");
    assert(dim_style.dim == true);
    std::println("PASS\n");
}

TEST_CASE("style predefined fg colors") {
    std::println("--- test_style_predefined_fg_colors ---");
    assert(fg_red.fg.has_value()   && fg_red.fg->fg_sgr()   == "31");
    assert(fg_green.fg.has_value() && fg_green.fg->fg_sgr() == "32");
    std::println("PASS\n");
}

TEST_CASE("theme owns_canvas: native defers, a scheme claims") {
    std::println("--- test_theme_owns_canvas ---");
    // The whole native/scheme split, decided by data rather than by name:
    // a theme owns the canvas exactly when it states a real background.
    assert(!theme::owns_canvas(theme::native));
    assert(theme::native.background.kind() == Color::Kind::Default);

    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    assert(theme::owns_canvas(scheme));
    std::println("PASS\n");
}

TEST_CASE("theme canvas fill is inline-safe") {
    std::println("--- test_theme_canvas_fill_inline_safe ---");
    // A host in Mode::Inline paints a window onto live scrollback, not the
    // whole screen. apply_theme_canvas() is only sound if the fill (a)
    // reaches the right edge, so there is no ragged tear where the
    // terminal's own background shows through the gaps, and (b) stops at
    // the content, so it never repaints rows that belong to the user's
    // history. Both are asserted because both are invisible in review and
    // only show up as someone's recoloured scrollback.
    using namespace maya::dsl;

    auto tree = [] { return v(text("hello"), text("a longer line")); };

    auto paint = [&](const Theme& t, auto&& fn) {
        StylePool pool;
        Canvas c{40, 6, &pool};
        render_tree(detail::apply_theme_canvas(Element{tree()}.build(), t,
                                               /*term_width=*/40),
                    c, pool, theme::native, /*auto_height=*/true);
        fn(c, pool);
    };

    auto has_bg = [](const Canvas& c, const StylePool& pool, int x, int y) {
        const Style& st = pool.get(c.get(x, y).style_id);
        return st.bg.has_value() && st.bg->kind() != Color::Kind::Default;
    };

    // native: the seam is a no-op. Nothing is filled, so the terminal's own
    // background — including transparency and wallpaper — survives.
    paint(theme::native, [&](const Canvas& c, const StylePool& pool) {
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                assert(!has_bg(c, pool, x, y));
    });

    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    paint(scheme, [&](const Canvas& c, const StylePool& pool) {
        const int last = c.max_content_row();
        assert(last == 1);
        // (a) full width on every content row, including past the text.
        for (int y = 0; y <= last; ++y)
            for (int x = 0; x < c.width(); ++x)
                assert(has_bg(c, pool, x, y));
        // (b) nothing below the content — that is scrollback.
        for (int y = last + 1; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                assert(!has_bg(c, pool, x, y));
    });

    std::println("PASS\n");
}

TEST_CASE("theme reaches markdown, not just the chrome") {
    std::println("--- test_theme_markdown_projection ---");
    // Markdown is most of what is on screen in a chat TUI — prose, code
    // spans, tables. A theme that repaints the borders but leaves the body
    // on its launch-time palette is the "it barely changed anything" bug,
    // so this asserts the projection actually moves.

    const MarkdownPalette nat = markdown_palette_from(theme::native);
    // native must stay terminal-native: body prose is the terminal's own
    // foreground, and the code background is whatever the terminal is.
    // Anything literal here would defeat the point of native.
    assert(nat.text.kind() == Color::Kind::Default);
    assert(nat.code_bg.kind() == Color::Kind::Default);

    Theme scheme = theme::native;
    scheme.text       = Color::rgb(0xF8, 0xF8, 0xF2);
    scheme.surface    = Color::rgb(0x34, 0x36, 0x41);
    scheme.border     = Color::rgb(0x56, 0x57, 0x5F);
    scheme.info       = Color::rgb(0x8B, 0xE9, 0xFD);
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);

    const MarkdownPalette p = markdown_palette_from(scheme);
    assert(p.text == scheme.text);
    assert(p.code_fg == scheme.info);
    assert(p.table_border == scheme.border);
    // The code background is the SURFACE slot, never a literal black: a
    // hardcoded black is a hole punched through a light scheme.
    assert(p.code_bg == scheme.surface);
    assert(p.code_bg != Color::black());

    std::println("PASS\n");
}

TEST_CASE("theme slot survives the runtime move") {
    std::println("--- test_theme_slot_survives_move ---");
    // THE bug behind "the theme recolours text but never the background".
    //
    // The slot used to be published inside Runtime::create(), against a
    // local that is then moved into Result<Runtime> and moved AGAIN into
    // the caller. The published pointer named storage that was dead before
    // the first frame, so every app_set_theme() wrote into freed memory and
    // rt.theme() — which is what the canvas fill is keyed on — never left
    // its startup value. Foreground colour still changed, because hosts
    // read their palette from their own published copy, which is exactly
    // why it looked like "the theme half works".
    //
    // This reproduces the shape without a terminal: publish against an
    // object, move it, and require that writes land where reads happen.
    struct Holder { Theme t = theme::native; };

    auto make = [] { return Holder{}; };
    Holder h = make();          // moved into place, as Runtime is

    Theme* slot = &h.t;         // published AFTER the move — the fix
    Theme scheme = theme::native;
    scheme.background = Color::rgb(0x28, 0x2A, 0x36);
    *slot = scheme;

    // A write through the slot must be visible to the object the renderer
    // actually reads, and must flip the canvas decision with it.
    assert(h.t.background == scheme.background);
    assert(theme::owns_canvas(h.t));
    assert(!theme::owns_canvas(theme::native));

    std::println("PASS\n");
}

TEST_CASE("theme slots resolve, and literals do not") {
    std::println("--- test_theme_slot_resolution ---");
    // A widget Config default is written once, at static-init, with no theme
    // in scope. Color::slot() lets it name a ROLE instead of a hue and be
    // answered at paint time — which is what stops ~250 such defaults from
    // pinning the UI to whatever palette it was compiled with.
    Theme t = theme::native;
    t.accent  = Color::rgb(0xBD, 0x93, 0xF9);
    t.muted   = Color::rgb(0x62, 0x72, 0xA4);
    t.error   = Color::rgb(0xFF, 0x55, 0x55);

    assert(t.resolve(Color::slot(ThemeSlot::Accent)) == t.accent);
    assert(t.resolve(Color::slot(ThemeSlot::Muted))  == t.muted);
    assert(t.resolve(Color::slot(ThemeSlot::Error))  == t.error);

    // A literal is an explicit host override and must survive untouched —
    // otherwise a caller could never escape the theme.
    const Color lit = Color::rgb(1, 2, 3);
    assert(t.resolve(lit) == lit);
    assert(t.resolve(Color::red()) == Color::red());
    assert(t.resolve(Color::default_color()) == Color::default_color());

    // Under native every slot lands on the terminal's own palette, so the
    // default look is unchanged by all of this.
    assert(theme::native.resolve(Color::slot(ThemeSlot::Muted))
           == theme::native.muted);

    std::println("PASS\n");
}

TEST_CASE("theme discipline: ink slots are never backgrounds") {
    std::println("--- test_theme_slot_axis ---");
    // A slot belongs to one of two AXES, and the axes move in opposite
    // directions when a theme flips polarity:
    //
    //   INK      — text and accents. Dark on a light theme, light on a dark
    //               one, because it must contrast the canvas.
    //   SURFACE  — fills and tints. Light on a light theme, dark on a dark
    //               one, because it IS (a step off) the canvas.
    //
    // Using an ink slot as a background inverts on the opposite polarity: a
    // "dark grey" fill written against a dark theme becomes a near-black bar
    // on a light one. That is invisible in review — both sides look
    // deliberate — and only shows up as a dark slab across someone's light
    // terminal. The bulk slot conversion introduced five of exactly this.
    //
    // So the rule is checked against the theme DATA, which is the only place
    // it can be checked honestly: for a light theme and a dark theme, every
    // surface slot must sit on the same side as its background, and every
    // ink slot on the opposite side.
    auto lum = [](Color c) {
        return 0.2126 * c.r() + 0.7152 * c.g() + 0.0722 * c.b();
    };

    int checked = 0;
    for (const auto& s : theme::schemes) {
        const Theme& t = *s.theme;
        if (t.background.kind() != Color::Kind::Rgb) continue;
        const bool light_bg = lum(t.background) > 128.0;
        ++checked;

        // Surface slots sit on the canvas's side. `shadow` is exempt: it is
        // meant to be darker than the canvas on BOTH polarities.
        for (const auto& [name, c] : {
                 std::pair{"surface", t.surface},
                 std::pair{"overlay", t.overlay},
                 std::pair{"inverse_text", t.inverse_text},
                 std::pair{"diff_added", t.diff_added},
                 std::pair{"diff_removed", t.diff_removed},
                 std::pair{"diff_changed", t.diff_changed}}) {
            if (c.kind() != Color::Kind::Rgb) continue;
            CHECK_MESSAGE((lum(c) > 128.0) == light_bg,
                          s.name << ": " << name
                                 << " is on the wrong side of the canvas");
        }

        // Text must contrast the canvas — the one guarantee legibility rests
        // on, and the thing a mis-assigned slot destroys.
        if (t.text.kind() == Color::Kind::Rgb)
            CHECK_MESSAGE((lum(t.text) > 128.0) != light_bg,
                          s.name << ": text does not contrast the background");
    }
    CHECK(checked > 0);
    std::println("PASS ({} schemes checked)\n", checked);
}

TEST_CASE("theme: a style with no background means THE CANVAS") {
    std::println("--- test_theme_bg_default ---");
    // The structural end of "some frame lines still show the terminal
    // through".
    //
    // Widgets produce bg-less styles constantly and legitimately — a border
    // glyph, a divider rule, a bare label. Those styles used to emit no
    // background SGR, which a terminal reads as "reset to MY default", so
    // every one of them punched a hole in a themed canvas. Fixing them
    // widget-by-widget never converged, because the DEFAULT was wrong
    // rather than any one widget: the next bg-less style anyone wrote
    // brought the bug straight back.
    //
    // Inverting the default ends the class. build_sgr and
    // write_transition_sgr are the ONLY two places a cell becomes bytes,
    // and both now render "no opinion" as the canvas colour. A widget can
    // no longer forget, because there is nothing left to remember.
    using namespace maya::dsl;

    auto emit_for = [](const Theme& t) {
        theme::set_live(t);
        StylePool pool;
        Canvas c{40, 4, &pool};
        pool.retheme();
        render_tree(v(text("header"), sep, text("row")).build(),
                    c, pool, t, /*auto_height=*/true);
        std::string out;
        serialize(c, pool, out);
        return out;
    };

    // A themed canvas: every row carries it, including the separator's
    // border glyphs and the blank tail past the text.
    Theme light = theme::native;
    light.background = Color::rgb(0xEA, 0xEA, 0xEA);
    const std::string themed = emit_for(light);
    assert(themed.find("48;2;234;234;234") != std::string::npos);

    // native states no background, so nothing is emitted and the terminal
    // shows through — transparency, background image and all. This is the
    // case an unconditional fill would have destroyed, and it is the whole
    // reason the rule is keyed on owns_canvas rather than applied always.
    const std::string nat = emit_for(theme::native);
    assert(nat.find("48;2") == std::string::npos);
    assert(nat.find("48;5") == std::string::npos);

    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("theme: markdown tables and code blocks carry the canvas") {
    std::println("--- test_theme_markdown_render ---");
    // Table rules and code-block frames are styled fg-only — with_fg(border)
    // and nothing else — because a border has no reason to name a
    // background. Under the old default that made every one of them a hole
    // in a themed canvas, which is what "even in md" looked like: a light
    // panel with the terminal showing through every rule and frame.
    //
    // This renders the two worst offenders through the real markdown
    // pipeline rather than a stand-in, because the bug was never in the
    // widgets — it was in what a bg-less style MEANT.
    Theme t   = theme::native;
    t.background = Color::rgb(0xEA, 0xEA, 0xEA);
    t.text       = Color::rgb(0x23, 0x23, 0x22);
    t.border     = Color::rgb(0xBE, 0xBE, 0xBE);
    t.surface    = Color::rgb(0xDE, 0xDE, 0xDE);
    t.info       = Color::rgb(0x0E, 0x71, 0x7C);
    theme::set_live(t);
    set_markdown_palette(markdown_palette_from(t));

    StylePool pool;
    Canvas c{60, 20, &pool};
    pool.retheme();

    StreamingMarkdown md;
    md.feed("| col | val |\n|---|---|\n| a | 1 |\n\n```cpp\nint x = 1;\n```\n");
    md.finish();
    render_tree(md.build(), c, pool, t, /*auto_height=*/true);

    std::string out;
    serialize(c, pool, out);

    // Walk the emitted bytes tracking background state; every glyph must
    // land on a stated background, never the terminal's.
    int holes = 0, painted = 0;
    bool def = true;
    for (std::size_t i = 0; i < out.size();) {
        if (out[i] == '\x1b') {
            const std::size_t j = out.find('m', i);
            if (j != std::string::npos && i + 1 < out.size() && out[i + 1] == '[') {
                const std::string ps = out.substr(i + 2, j - i - 2);
                if (ps.find("48;2") != std::string::npos) def = false;
                else if (ps.empty() || ps == "0"
                         || ps.find("49") != std::string::npos) def = true;
                i = j + 1;
                continue;
            }
            i += 2;
            continue;
        }
        if (out[i] != '\n' && out[i] != '\r' && out[i] != ' ') {
            if (def) ++holes; else ++painted;
        }
        ++i;
    }
    assert(painted > 0);
    assert(holes == 0);

    set_markdown_palette(markdown_palette_from(theme::native));
    theme::set_live(theme::native);
    std::println("PASS\n");
}

TEST_CASE("style equality") {
    std::println("--- test_style_equality ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_bold().with_fg(Color::red());
    Style c = Style{}.with_bold().with_fg(Color::green());
    assert(a == b);
    assert(a != c);
    std::println("PASS\n");
}

