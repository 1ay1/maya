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

/// Serialize `rows` rows of the canvas starting at `start_row`
/// (or all rows if rows <= 0).
void serialize(const Canvas& canvas, const StylePool& pool,
               std::string& out, int rows = 0, int start_row = 0);

/// Incremental serialize: only re-render rows whose hash differs between
/// old_hashes and new_hashes.  Unchanged rows are skipped with cursor
/// movement, dramatically reducing output on slow terminals.
///
/// old_offset: when the display scrolled (skip_rows changed between frames),
/// old_hashes[y] no longer corresponds to the same display position as
/// new_hashes[y].  Pass (prev_skip_rows - skip_rows) so the comparison
/// uses old_hashes[y + old_offset] — the hash of what was previously
/// displayed at the same physical row.
void serialize_changed(const Canvas& canvas, const StylePool& pool,
                       std::string& out, int rows, int start_row,
                       const uint64_t* old_hashes, int old_count,
                       const uint64_t* new_hashes, int new_count,
                       int old_offset = 0);

} // namespace maya
