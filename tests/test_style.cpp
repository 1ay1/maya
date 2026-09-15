// Tests for maya style system: Style attributes, SGR generation, merge, operator|
#include <maya/maya.hpp>
#include <maya/widget/markdown.hpp>   // markdown_palette_from
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
        render_tree(detail::apply_theme_canvas(Element{tree()}.build(), t),
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

TEST_CASE("style equality") {
    std::println("--- test_style_equality ---");
    Style a = Style{}.with_bold().with_fg(Color::red());
    Style b = Style{}.with_bold().with_fg(Color::red());
    Style c = Style{}.with_bold().with_fg(Color::green());
    assert(a == b);
    assert(a != c);
    std::println("PASS\n");
}

