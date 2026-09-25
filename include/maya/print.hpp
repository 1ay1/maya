#pragma once
// maya/print.hpp — render an element once, with no runtime and no terminal
// mode changes: Ink's renderToString. For styled CLI output (a report, a
// table, a status card) and for tests. An interactive or animated program
// is a jaal program run with maya::run (<maya/app.hpp>); an inline one is
// the same program with `.mode = Mode::Inline`.

#include <string>

#include "element/element.hpp"

namespace maya {

namespace detail {
int detect_terminal_width() noexcept;
int detect_terminal_height() noexcept;
}  // namespace detail

/// Render to stdout at the terminal's width (80 when it isn't a terminal).
void print(const Element& root);

/// Render to stdout at an explicit width.
void print(const Element& root, int width);

/// The rendered text, one line per row, no styling.
[[nodiscard]] std::string render_to_string(const Element& root, int width = 80);

/// Like render_to_string, but keeps styling as SGR sequences. A test aid:
/// it lets an assertion check colour and weight, not just text.
[[nodiscard]] std::string render_to_string_ansi(const Element& root, int width = 80);

}  // namespace maya
