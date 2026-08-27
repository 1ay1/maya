// maya::render::grid_emit — implementation.  See grid_emit.hpp for the wire
// format and rationale.
#include "maya/render/grid_emit.hpp"
#include "maya/render/diff.hpp"     // encode_utf8
#include "maya/style/style.hpp"
#include "maya/style/color.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace maya::render {

namespace {

// Wire protocol version.  v2 added the u32 length prefix in `wrap_apc' so the
// frame is self-delimiting regardless of payload bytes (v1 relied on scanning
// for the ESC \ terminator, which a binary payload could spoof).
constexpr std::uint8_t GRID_PROTO_VER = 2;

// ── little-endian appenders ────────────────────────────────────────────────
inline void put_u8 (std::string& o, std::uint8_t v)  { o.push_back(static_cast<char>(v)); }
inline void put_u16(std::string& o, std::uint16_t v) {
    o.push_back(static_cast<char>(v & 0xFF));
    o.push_back(static_cast<char>((v >> 8) & 0xFF));
}
inline void put_u32(std::string& o, std::uint32_t v) {
    o.push_back(static_cast<char>( v        & 0xFF));
    o.push_back(static_cast<char>((v >>  8) & 0xFF));
    o.push_back(static_cast<char>((v >> 16) & 0xFF));
    o.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// LEB128 unsigned varint: 7 data bits per byte, high bit = continuation.
// Values < 128 take 1 byte, < 16384 take 2. Grid run coordinates (row, col,
// len, style-id) are almost always small, so this halves the fixed u16x4 run
// header on typical content. Opt-in (kFlagVarintRuns) so the fixed path stays
// byte-identical for v2 hosts.
inline void put_varint(std::string& o, std::uint32_t v) {
    while (v >= 0x80) {
        o.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    o.push_back(static_cast<char>(v));
}

// A color tagged for the host to resolve.  Named/Default carry no true RGB
// (they follow the user's terminal/emacs theme), so we send the kind + index;
// Indexed/Rgb carry resolvable values.
inline void put_color(std::string& o, const std::optional<Color>& c) {
    if (!c) { put_u8(o, 0); return; }                 // 0 = Default/inherit
    switch (c->kind()) {
        case Color::Kind::Default: put_u8(o, 0); break;
        case Color::Kind::Named:   put_u8(o, 1); put_u8(o, c->index()); break;
        case Color::Kind::Indexed: put_u8(o, 2); put_u8(o, c->index()); break;
        case Color::Kind::Rgb:
            put_u8(o, 3); put_u8(o, c->r()); put_u8(o, c->g()); put_u8(o, c->b());
            break;
    }
}

inline std::uint16_t pack_attrs(const Style& s) {
    return static_cast<std::uint16_t>(
        (s.bold          ? 1u << 0 : 0) |
        (s.dim           ? 1u << 1 : 0) |
        (s.italic        ? 1u << 2 : 0) |
        (s.underline     ? 1u << 3 : 0) |
        (s.strikethrough ? 1u << 4 : 0) |
        (s.inverse       ? 1u << 5 : 0) |
        (s.conceal       ? 1u << 6 : 0));
}

// APC wrapper:  ESC _ G  <u32 payload_len LE>  <payload>  ESC \ .
//
// The payload is RAW BINARY (u16 fields, UTF-8 text) and can contain ANY byte
// — including the terminator pair ESC \ (0x1b 0x5c), a NUL, etc.  So the frame
// must NOT be delimited by scanning for ESC \: it is LENGTH-PREFIXED.  The host
// reads the u32 immediately after `ESC _ G`, consumes exactly that many payload
// bytes, and treats the trailing ESC \ as a sanity check only.  This makes the
// framing content-independent and safe across arbitrary pty chunk boundaries.
// (The u16 you'd otherwise appended is unchanged; only wrap_apc gained a
// length prefix.)
inline void wrap_apc(const std::string& payload, std::string& out) {
    out += "\x1b_G";
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;
    out += "\x1b\\";
}

// Per-frame style-table builder: assigns each distinct style_id used by the
// emitted runs a slot in the table (deduped), so the host interns each once.
struct StyleTable {
    // style_id → present.  We keep the maya style_id AS the wire id (stable
    // within a run of frames because StylePool is monotonic), so the host can
    // cache by it across frames and skip re-interning unchanged styles.
    std::vector<std::uint16_t> ids;
    std::unordered_map<std::uint16_t, bool> seen;

    void note(std::uint16_t id) {
        if (seen.emplace(id, true).second) ids.push_back(id);
    }
    [[nodiscard]] bool empty() const { return ids.empty(); }

    void write(const StylePool& pool, std::string& o) const {
        put_u16(o, static_cast<std::uint16_t>(ids.size()));
        for (std::uint16_t id : ids) {
            const Style& s = pool.get(id);
            put_u16(o, id);
            put_color(o, s.fg);
            put_color(o, s.bg);
            put_u16(o, pack_attrs(s));
        }
    }

    // Cross-frame dictionary write: emit DEFINITIONS only for styles the host
    // hasn't been told about yet (per `ack`); already-acked ids are omitted
    // (the runs still reference them by id, the host resolves from its cache).
    // Records every emitted id into `ack`. Returns true iff any definition was
    // withheld — i.e. the table is PARTIAL and the frame must set bit4.
    bool write_partial(const StylePool& pool, std::string& o,
                       StyleAckSet& ack) const {
        std::vector<std::uint16_t> fresh;
        fresh.reserve(ids.size());
        for (std::uint16_t id : ids)
            if (!ack.has(id)) fresh.push_back(id);
        put_u16(o, static_cast<std::uint16_t>(fresh.size()));
        for (std::uint16_t id : fresh) {
            const Style& s = pool.get(id);
            put_u16(o, id);
            put_color(o, s.fg);
            put_color(o, s.bg);
            put_u16(o, pack_attrs(s));
            ack.note(id);
        }
        return fresh.size() != ids.size();   // some ids were withheld
    }
};

// Build the run list for one row into `runs_out`, noting styles.  A run is a
// maximal same-style span.  Wide-glyph trailing halves (width==2) are folded
// into their lead codepoint's run: we emit the codepoint once and skip the
// trailing-half column, matching how the host lays a wide glyph across cells.
struct Run { std::uint16_t row, col, len, style; std::string utf8; };

// A cell's character must never reach the wire as invalid UTF-8: NUL, a UTF-16
// surrogate, or an out-of-range code point would make the host's decoder
// produce mojibake or (pre-length-prefix) desync the stream.  Map anything
// ill-formed to U+FFFD.  A blank cell (NUL from a raw memset path) becomes a
// space so the column still occupies one cell.
inline char32_t sanitize_cp(char32_t cp) {
    if (cp == 0) return U' ';
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;   // lone surrogate
    if (cp > 0x10FFFF) return 0xFFFD;                   // out of Unicode range
    return cp;
}

// Set once at startup from AGENTTY_GRID_WIDTH_RESOLVED=1. When true, wide
// glyphs are emitted with a U+0000 CONTINUATION codepoint filling their
// trailing column, so a run's codepoint count equals its COLUMN count. A host
// then advances one column per codepoint and renders U+0000 as nothing — no
// wcwidth on the host side, no column drift. Off by default (v2 behaviour:
// trailing half is skipped, len = codepoints < columns).
inline bool width_resolved_mode() {
    static const bool on = [] {
        const char* e = std::getenv("AGENTTY_GRID_WIDTH_RESOLVED");
        return e && e[0] == '1';
    }();
    return on;
}

void build_row_runs(const Canvas& canvas, int row, std::vector<Run>& out,
                    StyleTable& table, int col_lo = 0) {
    const bool resolved = width_resolved_mode();
    const int w = canvas.width();
    // `col_lo` restricts emission to columns >= col_lo. A Diff frame is an
    // OVERLAY keyed by (row, col) on the host, so columns before the first
    // changed one already hold the right cells — emitting only the changed
    // suffix is byte-identical on screen but far smaller on the wire, which is
    // the dominant per-frame cost during the streaming glide (only a row's
    // tail columns change each frame). col_lo=0 reproduces full-row behaviour.
    int x = std::max(0, col_lo);
    // If col_lo landed on the TRAILING half of a wide glyph, back up to its
    // lead cell so we never emit a dangling continuation / half a glyph.
    if (x > 0 && x < w && canvas.get(x, row).width == 2) --x;
    while (x < w) {
        Cell c = canvas.get(x, row);
        if (c.width == 2) { ++x; continue; }        // trailing half; folded already
        const std::uint16_t style = c.style_id;
        Run run{static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(x),
                0, style, {}};
        // extend while same style
        int cx = x;
        while (cx < w) {
            Cell cc = canvas.get(cx, row);
            if (cc.width == 2) { ++cx; continue; }
            if (cc.style_id != style) break;
            maya::detail::encode_utf8(sanitize_cp(cc.character), run.utf8);
            ++run.len;
            ++cx;
            // Width-resolved: a wide lead glyph (its trailing half is the next
            // width==2 cell) gets a U+0000 continuation so len tracks columns.
            if (resolved && cx < w && canvas.get(cx, row).width == 2) {
                run.utf8.push_back('\0');   // U+0000, one byte, valid UTF-8
                ++run.len;
            }
        }
        table.note(style);
        out.push_back(std::move(run));
        x = cx;
    }
}

void write_runs(const std::vector<Run>& runs, std::string& o,
                bool varint = false) {
    put_u16(o, static_cast<std::uint16_t>(runs.size()));
    if (varint) {
        for (const Run& r : runs) {
            put_varint(o, r.row);
            put_varint(o, r.col);
            put_varint(o, r.len);
            put_varint(o, r.style);
            o += r.utf8;
        }
    } else {
        for (const Run& r : runs) {
            put_u16(o, r.row);
            put_u16(o, r.col);
            put_u16(o, r.len);
            put_u16(o, r.style);
            o += r.utf8;
        }
    }
}

// Common frame builder for Diff/Full.
// `changed_cols`, when non-null, is index-aligned with `rows`: entry i is the
// first column of rows[i] that differs from the host's current frame, so a Diff
// only emits each row's changed suffix (huge per-frame savings during the
// streaming glide). Null / Full frames emit whole rows (col_lo = 0).
void emit_cells(const Canvas& canvas, const StylePool& pool,
                const std::vector<int>& rows, int base_row,
                GridFrameType type, const GridCursor* cursor, std::string& out,
                int header_rows = -1,
                const std::vector<int>* changed_cols = nullptr,
                StyleAckSet* ack = nullptr) {
    StyleTable table;
    std::vector<Run> runs;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int row = rows[i];
        if (row < 0 || row >= canvas.height()) continue;
        const int col_lo = (changed_cols && i < changed_cols->size())
                               ? (*changed_cols)[i] : 0;
        build_row_runs(canvas, row, runs, table, col_lo);
    }

    std::string p;
    // With an ack set the table may be PARTIAL: it defines only styles the
    // host hasn't seen. Even when every style is already acked (0 fresh
    // defs) the section is still emitted (as count=0) and bit0 stays set so
    // the host knows to parse it — the runs reference cached ids. bit4 flags
    // "partial" so the host resolves absent ids from its cache. Rendered to
    // a scratch buffer first because whether bit4 is set depends on what the
    // dictionary write withholds.
    std::string table_bytes;
    bool partial = false;
    if (!table.empty()) {
        if (ack) partial = table.write_partial(pool, table_bytes, *ack);
        else     table.write(pool, table_bytes);
    }
    const bool varint = ack && ack->varint_runs;
    const std::uint8_t flags =
        (table.empty() ? 0u : 1u) | (cursor ? 2u : 0u)
        | (width_resolved_mode() ? 8u : 0u)   // bit3 = width-resolved runs
        | (partial ? kFlagPartialStyleTable : 0u)   // bit4 = partial dict
        | (varint  ? kFlagVarintRuns : 0u);         // bit5 = varint run headers
    put_u8 (p, GRID_PROTO_VER);                      // ver
    put_u8 (p, static_cast<std::uint8_t>(type));
    put_u8 (p, flags);
    put_u8 (p, 0);                                   // reserved
    put_u16(p, static_cast<std::uint16_t>(canvas.width()));
    // header `rows`: the canvas height, OR an explicit override (Commit frames
    // put their COMMIT COUNT here instead of a grid height).
    put_u16(p, static_cast<std::uint16_t>(
                   header_rows >= 0 ? header_rows : canvas.height()));
    put_u16(p, static_cast<std::uint16_t>(base_row));
    p += table_bytes;
    write_runs(runs, p, varint);
    if (cursor) {
        put_u16(p, static_cast<std::uint16_t>(cursor->row));
        put_u16(p, static_cast<std::uint16_t>(cursor->col));
        put_u8 (p, cursor->visible ? 1 : 0);
    }
    wrap_apc(p, out);
}

// Emit committed rows as a Commit frame (count in the header rows field).
void emit_commit_rows_impl(const Canvas& canvas, const StylePool& pool,
                           const std::vector<int>& rows, int count,
                           int base_row, std::string& out) {
    emit_cells(canvas, pool, rows, base_row, GridFrameType::Commit,
               nullptr, out, /*header_rows=*/count);
}

// Header-only frames (Resize/Cursor/Clear/Bell) share this skeleton.
void emit_header_only(GridFrameType type, int cols, int rows,
                      const GridCursor* cursor, std::string& out) {
    std::string p;
    const std::uint8_t flags = (cursor ? 2u : 0u);
    put_u8 (p, GRID_PROTO_VER);
    put_u8 (p, static_cast<std::uint8_t>(type));
    put_u8 (p, flags);
    put_u8 (p, 0);
    put_u16(p, static_cast<std::uint16_t>(cols));
    put_u16(p, static_cast<std::uint16_t>(rows));
    put_u16(p, 0);
    // no style table, no runs
    put_u16(p, 0);                                   // nruns = 0
    if (cursor) {
        put_u16(p, static_cast<std::uint16_t>(cursor->row));
        put_u16(p, static_cast<std::uint16_t>(cursor->col));
        put_u8 (p, cursor->visible ? 1 : 0);
    }
    wrap_apc(p, out);
}

} // namespace

void emit_diff(const Canvas& canvas, const StylePool& pool,
               const std::vector<int>& changed_rows, int base_row,
               const GridCursor* cursor, std::string& out,
               const std::vector<int>* changed_cols,
               StyleAckSet* ack) {
    emit_cells(canvas, pool, changed_rows, base_row, GridFrameType::Diff,
               cursor, out, /*header_rows=*/-1, changed_cols, ack);
}

void emit_full(const Canvas& canvas, const StylePool& pool,
               int base_row, const GridCursor* cursor, std::string& out,
               StyleAckSet* ack) {
    std::vector<int> all(static_cast<std::size_t>(std::max(0, canvas.height())));
    for (int i = 0; i < canvas.height(); ++i) all[static_cast<std::size_t>(i)] = i;
    // A Full frame re-states the whole surface: the host's style cache is no
    // longer trustworthy relative to it, so reset the ack set and re-send
    // every definition (write_partial on a cleared set emits all, marks none
    // withheld → the frame is a complete v2-shaped table again).
    if (ack) ack->reset();
    emit_cells(canvas, pool, all, base_row, GridFrameType::Full, cursor, out,
               /*header_rows=*/-1, /*changed_cols=*/nullptr, ack);
}

void emit_resize(int cols, int rows, std::string& out) {
    emit_header_only(GridFrameType::Resize, cols, rows, nullptr, out);
}

void emit_cursor(const GridCursor& cursor, int cols, int rows, std::string& out) {
    emit_header_only(GridFrameType::Cursor, cols, rows, &cursor, out);
}

void emit_clear(int cols, int rows, std::string& out) {
    emit_header_only(GridFrameType::Clear, cols, rows, nullptr, out);
}

void emit_bell(std::string& out) {
    emit_header_only(GridFrameType::Bell, 0, 0, nullptr, out);
}

void emit_commit(int rows, std::string& out) {
    // The commit COUNT rides the `rows` header field; cols is irrelevant.
    emit_header_only(GridFrameType::Commit, 0, rows, nullptr, out);
}

void emit_commit_rows(const Canvas& canvas, const StylePool& pool,
                      const std::vector<int>& rows, int count, int base_row,
                      std::string& out) {
    emit_commit_rows_impl(canvas, pool, rows, count, base_row, out);
}

} // namespace maya::render
