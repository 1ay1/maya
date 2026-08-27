#pragma once
// maya::render::grid_emit — the "grid protocol" host backend.
//
// When maya runs on a COOPERATING HOST that can paint a cell grid natively
// (an Emacs dynamic module, a future GPU frontend, …) we skip the ANSI round
// trip entirely.  A normal terminal receives maya's diff as ANSI escapes,
// which the terminal must RE-PARSE back into a cell grid before it can draw.
// A cooperating host already speaks cells — so instead of serialize()-ing the
// Canvas to ANSI we emit the SAME row-level diff as a compact binary frame the
// host applies directly.  No ANSI encode on our side, no ANSI parse on theirs.
//
// Transport: each frame is wrapped in an APC string
//
//     ESC _ G <binary> ESC \        (0x1b 0x5f 'G' … 0x1b 0x5c)
//
// so it rides the SAME pty/stdout as everything else.  A terminal that isn't
// the host treats it as an unknown APC and ignores it (no corruption); the
// host scans for `ESC _ G`, reads the payload length, decodes exactly that many
// bytes, and never involves a terminal emulator.  Grid mode REPLACES ANSI
// frame output — it is chosen at startup (RunConfig / env), not mixed per frame.
//
// Wire envelope (v2):
//
//   ESC _ G  <u32 payload_len LE>  <payload>  ESC \
//
// The payload is RAW BINARY and may contain any byte, INCLUDING the terminator
// pair ESC \ (0x1b 0x5c) and NUL.  The frame is therefore LENGTH-PREFIXED, not
// terminator-scanned: the host reads the u32 right after `ESC _ G`, consumes
// exactly payload_len bytes, and uses the trailing ESC \ only as a sanity
// check.  (v1 had no length prefix and relied on scanning for ESC \, which a
// binary payload could spoof — corrupting the frame and everything after it.)
//
// Payload format (little-endian, versioned).  All multi-byte ints LE.
//
//   frame := header  [style_table]  cell_runs  [cursor]
//
//   header:
//     u8   ver        = 2
//     u8   type       0=DIFF 1=FULL 2=RESIZE 3=CURSOR 4=CLEAR 5=BELL
//     u8   flags      bit0 has_style_table · bit1 has_cursor
//     u8   reserved   = 0
//     u16  cols       (columns of the grid this frame targets)
//     u16  rows       (rows;  FULL/RESIZE define the surface, DIFF re-states)
//     u16  base_row   first grid row this frame's runs address (inline scroll
//                     anchor; 0 in fullscreen).  Lets the host place a partial
//                     diff without a full-surface reference.
//
//   style_table (if flag bit0):
//     u16  n
//     n ×  { u16 id · color fg · color bg · u16 attrs }
//       color := u8 kind (0=Default 1=Named16 2=Indexed256 3=Rgb) then
//                kind==1|2 → u8 index ;  kind==3 → u8 r,g,b ;  kind==0 → (none)
//       attrs bits: 0 bold 1 dim 2 italic 3 underline 4 strike 5 inverse 6 conceal
//
//   cell_runs:
//     u16  n
//     n ×  { u16 row · u16 col · u16 len · u16 style_id · utf8_bytes(len cps) }
//       A run is a maximal span of same-style cells on one row.  `len` counts
//       CODEPOINTS; the following bytes are their UTF-8, so a wide glyph is one
//       codepoint occupying (per the host's width rules) its columns.  A run's
//       trailing spaces are kept (they carry the style / clear intent).
//
//   cursor (if flag bit1):  u16 row · u16 col · u8 visible
//
// The emitter is a pure function of (Canvas, StylePool, diff rows): it owns no
// state.  The caller (the grid Writer) supplies which rows changed — the same
// row set the inline pipeline already computes — so we never re-scan clean
// rows.  Style ids are deduped per frame into the table so the host interns
// each style→face exactly once.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "canvas.hpp"

namespace maya::render {

// One emitted frame's payload type.
enum class GridFrameType : std::uint8_t {
    Diff   = 0,   // only the rows in `changed_rows`
    Full   = 1,   // the whole surface (initial / hard reset)
    Resize = 2,   // dimensions changed; carries no runs
    Cursor = 3,   // cursor moved only
    Clear  = 4,   // wipe the surface
    Bell   = 5,   // audible/visual bell passthrough
    Commit = 6,   // scrollback: the top N rows have scrolled off into
                  // history; the host appends its current top N grid rows
                  // to its scrollback and shifts the live grid up by N.
                  // The row COUNT rides the `rows` header field.
};

// Cursor position + visibility for a frame that carries one.
struct GridCursor {
    int  row     = 0;
    int  col     = 0;
    bool visible = true;
};

// Cross-frame style dictionary (opt-in). A streaming glide re-uses the same
// handful of styles (default face, bold, code span) every frame, but the v2
// style table re-sends each style's FULL definition (id + fg + bg + attrs ≈
// 9-11 B) every frame it appears — pure redundancy the host already caches by
// id. Passing a StyleAckSet to emit_diff/emit_full makes the emitter send a
// style's DEFINITION only the first time the host sees it; thereafter runs
// reference it by id alone and the definition is omitted. The frame sets
// flags bit4 (kFlagPartialStyleTable) so a cooperating host knows an id
// referenced by a run but absent from this frame's table means "use the one
// I sent you earlier."
//
// SAFETY / COMPAT: this is strictly opt-in. With ack==nullptr (the default)
// the encoder is byte-identical to v2 — no deployed host is affected. A host
// advertises support out-of-band before the caller starts passing an ack set;
// until then the field stays null and every frame is self-contained.
struct StyleAckSet {
    std::unordered_set<std::uint16_t> acked;
    // Cooperating-host capability: when true the emitter also encodes run
    // headers (row/col/len/style) as LEB128 VARINTS instead of fixed u16x4,
    // cutting the 8-byte header to ~4 for typical (small) coordinates — the
    // MEASURED dominant grid cost. Frame sets flags bit5 (kFlagVarintRuns) so
    // the host decodes runs as varints. Carried here (not a separate arg) so
    // one opt-in object gates the whole v3 cooperating-host feature set.
    bool varint_runs = false;
    // Reset when the host loses sync (Full/Clear/Resize re-state): the host's
    // style cache is only valid relative to the frames it actually received,
    // so a hard re-state must re-send definitions.
    void reset() { acked.clear(); }
    [[nodiscard]] bool has(std::uint16_t id) const { return acked.count(id) != 0; }
    void note(std::uint16_t id) { acked.insert(id); }
};

// flags bit4: this frame's style table is PARTIAL — it defines only styles the
// host hasn't been told about yet; other referenced ids come from its cache.
inline constexpr std::uint8_t kFlagPartialStyleTable = 0x10;

// flags bit5: this frame's run headers are LEB128 VARINTs (row, col, len,
// style) instead of fixed little-endian u16x4. Halves the per-run header on
// typical content. Opt-in via StyleAckSet::varint_runs.
inline constexpr std::uint8_t kFlagVarintRuns = 0x20;

// Encode `canvas` rows listed in `changed_rows` (must be sorted, in-range) as a
// DIFF frame, APC-wrapped, appended to `out`.  `base_row` is the inline scroll
// anchor (0 in fullscreen).  When `cursor` is set it is appended.  Styles used
// by the emitted runs are collected into the frame's style table.
//
// `changed_cols`, when non-null, must be index-aligned with `changed_rows`:
// entry i is the first column of changed_rows[i] that differs from the host's
// current frame. The Diff then emits only each row's changed SUFFIX (columns
// >= that column) — a Diff frame is an (row,col) overlay on the host, so the
// earlier columns keep their existing cells. This is byte-identical on screen
// but slashes per-frame wire size during the streaming glide, where only a
// row's tail columns change. Pass null (default) to emit whole rows.
void emit_diff(const Canvas& canvas, const StylePool& pool,
               const std::vector<int>& changed_rows, int base_row,
               const GridCursor* cursor, std::string& out,
               const std::vector<int>* changed_cols = nullptr,
               StyleAckSet* ack = nullptr);

// Encode the WHOLE canvas as a FULL frame (initial paint / hard reset).
// A Full frame re-states the surface, so it also RESETS the ack set (the
// host's style cache is meaningless across a hard re-state) and re-sends
// every referenced style definition.
void emit_full(const Canvas& canvas, const StylePool& pool,
               int base_row, const GridCursor* cursor, std::string& out,
               StyleAckSet* ack = nullptr);

// A dimension change: cols×rows, no runs.  The host resizes its grid.
void emit_resize(int cols, int rows, std::string& out);

// Cursor-only move.
void emit_cursor(const GridCursor& cursor, int cols, int rows, std::string& out);

// Clear the surface.
void emit_clear(int cols, int rows, std::string& out);

// Bell.
void emit_bell(std::string& out);

// Scrollback commit: the top `rows` rows have scrolled into history.  Carries
// no runs; the host moves its current top `rows` grid rows into scrollback.
void emit_commit(int rows, std::string& out);

// Scrollback commit WITH content: emit the given canvas `rows` as cell runs in
// a Commit frame (count = `count` in the header).  The host appends these exact
// rows to its scrollback (they scroll off the live viewport permanently).
// base_row makes the run row numbers viewport-relative like emit_diff.
void emit_commit_rows(const Canvas& canvas, const StylePool& pool,
                      const std::vector<int>& rows, int count, int base_row,
                      std::string& out);

} // namespace maya::render
