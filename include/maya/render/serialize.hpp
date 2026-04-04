#pragma once
// maya::render::serialize - Full canvas serialization to ANSI
//
// Converts every cell in the canvas to an ANSI byte stream — no diffing,
// no skipping, no front/back comparison. This is the approach used by Ink
// (and Claude Code): render fresh, erase previous output, write new output.
//
// Output format (row-major, left-to-right, top-to-bottom):
//   [SGR transition] [glyph] ... \r\n
//   [SGR transition] [glyph] ... \r\n
//   ...
//   \x1b[0m   (final SGR reset)
//
// The caller wraps this in sync_start / sync_end and precedes it with
// erase_lines(prev_height) to atomically replace the previous render.

#include <cstdint>
#include <string>

#include "canvas.hpp"
#include "../terminal/ansi.hpp"

namespace maya {

/// Find the last non-empty row in a canvas (1-based height).
/// Returns the number of rows that contain visible content.
int content_height(const Canvas& canvas) noexcept;

/// Serialize `rows` rows of the canvas (or all rows if rows <= 0).
void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows = 0);

} // namespace maya
