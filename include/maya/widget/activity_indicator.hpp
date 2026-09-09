#pragma once
// maya::widget::ActivityIndicator — single-row hex-dump tape.
//
// One row that looks like a live memory window — offset pointer, hex
// bytes, xxd-style ASCII gutter — and IS one: every byte shown comes
// from a real stream the host is party to. Three modes, by data:
//
//   WRITE  (Config::stream non-empty)  The newest bytes of an arriving
//          stream (model output), right-anchored on the write head.
//          Motion comes from DATA ARRIVAL — each new byte shifts the
//          window — so tape speed IS stream speed. The offset column is
//          the true cumulative byte count (an odometer, counting up),
//          the last couple of bytes tumble while "settling", and the
//          word currently being written carries the accent highlight.
//
//   READ   (Config::context non-empty, stream empty)  A read head
//          scanning REAL context bytes (the prompt the model is
//          reading) at a steady cadence. The window trails the head,
//          the offset column is the true read position within the
//          context (counting up, looping at the end), and the word
//          under the head lights up as it's passed — the input-side
//          mirror of WRITE. No tumble: these bytes are settled fact.
//
//   STATIC (both empty)  Channel static: deterministic splitmix noise
//          with bit-drift, offset pinned at 0x000000. Never dressed as
//          information — it reads as "no signal", which is the truth.
//
// A scanline highlight sweeps across the row independently in every
// mode, like a debugger read-head inspecting whichever byte it's over.
//
//   0x0001a2  74 68 65 20 72 65 74 72  |the retr|
//                          ^^^^^^^^^^     ^^^^    <- word being written
//
// Content-agnostic: the host decides which streams to narrate.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../app/app.hpp"        // Element plumbing (dsl)
#include "../core/motion.hpp"     // anim::default_clock / keep_animating_after
#include "../dsl.hpp"
#include "../element/element.hpp"
#include "../element/text.hpp"   // StyledRun, TextElement
#include "../style/color.hpp"

namespace maya {

class ActivityIndicator {
public:
    struct Config {
        Color       edge_color = Color::cyan();
        std::string detail;          // optional trailing token ("3.4s")
        // ── Write mode: live output stream ────────────────────
        // `stream` is a tail window of REAL bytes the host is receiving
        // (model output, reasoning, a compaction summary — whatever
        // the row is narrating), and `stream_total` is the true cumulative
        // byte count of that stream (>= stream.size()). When non-empty,
        // the tape is a hexdump of these bytes, right-anchored on the
        // newest byte: the offset column is the write-head odometer —
        // the TRUE cumulative byte count, counting UP as bytes arrive —
        // the hex column shows the actual UTF-8, and the ASCII gutter
        // shows the printable characters — i.e. the words that surface
        // are words the model is actually writing, moments before the
        // reveal animation shows them as prose. Newly-arrived bytes
        // tumble briefly before locking so arrival itself is visible.
        std::string_view stream;
        std::size_t      stream_total = 0;
        // ── Read mode: context being consumed ──────────────────
        // Used when `stream` is empty (the TTFT window / between
        // sub-turns): REAL input bytes the model is reading — the
        // user's prompt. A read head advances through them at a steady
        // cadence; the visible window trails it, the offset column is
        // the head's true position in the context, and the word under
        // the head carries the highlight. Both empty → STATIC (noise,
        // offset 0x000000) — honest "no signal".
        //
        // Lifetime: views must stay valid for this frame (build()
        // copies what it needs).
        std::string_view context;

        // ── Simple mode ────────────────────────────────────────
        // The byte tape narrates the wire honestly, but it reads as
        // noise to anyone who isn't debugging the transport: a row of
        // hex that changes every frame looks like a fault, not like
        // progress. Simple mode drops the tape entirely and renders
        // ONE calm status row — spinner + verb + detail — that stays
        // put for the whole run and only ever changes the things that
        // actually changed (the spinner frame and the elapsed clock).
        //
        // It is deliberately NOT a separate widget: the host swaps a
        // bool, and the height contract (exactly one row, always) is
        // identical in both modes, so the mode can flip mid-run
        // without a frame-height change.
        //
        // `verb` is the whole message ("thinking", "running grep").
        // `spinner` is one pre-picked glyph — the HOST owns the
        // animation clock so every animated surface in the frame
        // steps in lockstep; passing a glyph rather than spinning
        // internally keeps this widget a pure function of its config.
        // Empty spinner renders a static bullet (no animation at all).
        bool        simple = false;
        std::string verb;
        std::string spinner;
    };

    explicit ActivityIndicator(Config c) : cfg_(std::move(c)) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        // ── Simple mode: one calm, responsive row. ────────────────
        // Rendered BEFORE any tape work so none of the byte-window
        // machinery runs (no hex buffers, no read-head arithmetic).
        //
        // Responsive by construction: the row is a single text run
        // sized inside a component() so it re-fits on every resize,
        // and it degrades by DROPPING the detail (the elapsed/rate
        // readout) before it ever truncates the verb — at any width
        // the answer to "what is it doing" survives; only the
        // decoration goes. Below that it ellipsizes the verb rather
        // than wrapping, because wrapping would make the row two
        // rows tall and break the one-row height contract that keeps
        // the indicator→content flip from shifting the frame.
        if (cfg_.simple) {
            const Color muted = Color::bright_black();
            const Color accent = cfg_.edge_color;

            // No verb and no spinner => the pure SPACER form: one empty
            // row. The host uses this to hold the slot's height across the
            // active->settled seam without printing anything. Note this is
            // NOT the same as an empty tape Config, which renders STATIC
            // mode (a row of 0x000000) — an easy and ugly mistake.
            if (cfg_.verb.empty() && cfg_.spinner.empty())
                return text("").build();

            std::string spin  = cfg_.spinner.empty()
                                    ? std::string{"\xe2\x80\xa2"}   // •
                                    : cfg_.spinner;
            std::string verb   = cfg_.verb.empty() ? std::string{"working"}
                                                   : cfg_.verb;
            std::string detail = cfg_.detail;

            return component([=](int avail_w, int /*h*/) -> Element {
                using namespace dsl;
                const int indent   = 2;
                const int spin_w   = string_width(spin) + 1;   // glyph + space
                const int det_w    = detail.empty()
                                       ? 0 : string_width(detail) + 3;  // " · "
                const int verb_w   = string_width(verb);

                // Widest form that fits, in order of what matters.
                const bool with_detail =
                    !detail.empty() && indent + spin_w + verb_w + det_w <= avail_w;

                std::string s;
                std::vector<StyledRun> runs;
                auto put = [&](std::string_view t, Style st) {
                    if (t.empty()) return;
                    runs.push_back({s.size(), t.size(), st});
                    s += t;
                };

                put("  ", Style{});
                put(spin, Style{}.with_fg(accent));
                put(" ", Style{});

                // Ellipsize the verb only when even the bare form
                // overflows — a 20-column terminal still says what's
                // happening, just shorter.
                const int room = avail_w - indent - spin_w
                               - (with_detail ? det_w : 0);
                if (verb_w > room && room > 1) {
                    std::string cut = verb.substr(0, static_cast<std::size_t>(
                        std::max(0, room - 1)));
                    put(cut, Style{}.with_fg(muted));
                    put("\xe2\x80\xa6", Style{}.with_fg(muted));   // …
                } else {
                    put(verb, Style{}.with_fg(muted));
                }

                if (with_detail) {
                    put(" \xc2\xb7 ", Style{}.with_fg(muted).with_dim());
                    put(detail, Style{}.with_fg(muted).with_dim());
                }

                return Element{TextElement{
                    .content = std::move(s),
                    .style   = Style{},
                    .wrap    = TextWrap::NoWrap,
                    .runs    = std::move(runs),
                }};
            }).build();
        }

        // Absolute shared animation clock (clamped at 0): the tape is a
        // pure function of it, every instance in the process scrolls in
        // lockstep, and a frozen test clock pins the frame exactly.
        const std::int64_t now_ms =
            std::max<std::int64_t>(0, ::maya::anim_now_ms());

        const Color muted     = Color::bright_black();
        const Color highlight = cfg_.edge_color;
        const Color sweep_fg  = Color::white();
        // Real (non-head) text: brighter than the noise channel, cooler than
        // the head-word highlight — see style_for's role 3.
        const Color text_fg   = Color::bright_white();

        // ── Timing knobs.
        //   kReadMs    ms per 1-byte advance of the READ head.
        //   kDriftMs   ms per bit-flip on static bytes (continuous mutation).
        //   kSweepMs   ms per 1-column sweep of the scanline highlight.
        constexpr int kReadMs   = 140;
        constexpr int kDriftMs  =  60;
        constexpr int kSweepMs  =  90;

        // Every visible change steps on one of the cadences above.
        // Wake the loop exactly at the NEAREST upcoming step boundary
        // instead of unconditionally at ~60 fps — the tape renders the
        // same frames it always did, the loop just sleeps between them.
        // (WRITE-mode frames additionally arrive with the data — the
        // host repaints on byte arrival via its own render gating.)
        {
            std::int64_t until_next = kDriftMs;   // finest cadence bound
            for (int step : {kReadMs, kDriftMs, kSweepMs}) {
                const std::int64_t u = step - (now_ms % step);
                if (u < until_next) until_next = u;
            }
            anim::keep_animating_after(until_next > 0 ? until_next : 1);
        }

        // READ-head position on its steady cadence.
        const std::int64_t scroll = now_ms / kReadMs;

        // splitmix64-ish base hash of a stream position.
        auto base_byte = [](std::int64_t pos) -> std::uint8_t {
            std::uint64_t x = static_cast<std::uint64_t>(pos + 1)
                              * 0x9E3779B97F4A7C15ull;
            x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27; x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return static_cast<std::uint8_t>(x & 0xff);
        };
        // Bit-drift overlay: 0–2 flipped bits per (drift_tick, pos).
        // Bytes mutate continuously instead of fully re-randomizing.
        const std::int64_t drift_tick = now_ms / kDriftMs;
        auto drift_byte = [drift_tick](std::int64_t pos) -> std::uint8_t {
            std::uint64_t a = static_cast<std::uint64_t>(drift_tick)
                              * 0xD1B54A32D192ED03ull
                            ^ static_cast<std::uint64_t>(pos + 17)
                              * 0xA13FC965B91E1709ull;
            a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33;
            const std::uint8_t bit1 = static_cast<std::uint8_t>(1u << ((a >> 0) & 7));
            const std::uint8_t bit2 = static_cast<std::uint8_t>(1u << ((a >> 8) & 7));
            const bool hold = ((a >> 16) & 0x3) == 0;   // ~25% of slots hold a beat
            return hold ? 0u : static_cast<std::uint8_t>(bit1 ^ bit2);
        };

        // Mode select — strictly by what data exists.
        const bool write_mode = !cfg_.stream.empty();
        const bool read_mode  = !write_mode && !cfg_.context.empty();

        // Offset pointer — a TRUE position in every mode.
        //   WRITE : cumulative bytes received (odometer, counts up).
        //   READ  : the read head's position in the context, advancing
        //           1 byte per kReadMs and looping — an honest "model
        //           is consuming this" needle. (The old render here was
        //           a fake countdown — the exact critique that triggered
        //           this redesign.)
        //   STATIC: 0x000000 — no stream, no position, no pretending.
        // 6 hex digits in all modes so the chrome never shifts at a
        // mode flip.
        const std::size_t ctx_len = cfg_.context.size();
        // READ head position. The scan used to be `scroll % ctx_len`, which
        // teleports: on reaching the last byte the head snaps back to 0 and
        // the whole window jump-cuts to unrelated text. On a short prompt
        // that lands every few seconds, and it reads as the indicator
        // RESETTING mid-thought rather than working.
        //
        // Ping-pong instead — walk forward to the end, then back to the
        // start, over a 2*len cycle. Every step is +-1 byte from the last,
        // so the window always moves CONTINUOUSLY and the tape looks like
        // something re-reading a document rather than restarting one.
        const std::size_t read_head = [&]() -> std::size_t {
            if (!read_mode || ctx_len == 0) return 0;
            const std::int64_t len   = static_cast<std::int64_t>(ctx_len);
            if (len == 1) return 0;
            const std::int64_t cycle = 2 * len - 2;      // fwd + back, ends once
            std::int64_t phase = scroll % cycle;
            if (phase < 0) phase += cycle;
            return static_cast<std::size_t>(phase < len ? phase
                                                        : cycle - phase);
        }();
        char offbuf[20];
        // Snapshot the narrated bytes. <= a few hundred bytes (stream is a
        // host-capped tail; context is a prompt the host caps); copying makes
        // the component self-contained (the host's views only promise this
        // frame). Declared before the odometer because the WRITE offset is
        // derived from the glided head below, not from the raw total.
        std::string stream_tail{cfg_.stream};
        std::string context{cfg_.context};
        const std::size_t stream_total = cfg_.stream_total;

        // WRITE-mode window anchor: which absolute stream byte sits at the
        // right edge this frame.
        //
        // Anchoring hard on the newest byte makes motion purely ARRIVAL-
        // driven, and a real SSE wire does not arrive smoothly: a fat delta
        // lands, then several frames pass with nothing. The window sat
        // FROZEN through those frames and then jumped a whole phrase at once
        // — stutter, not flow ("it's not continuous"), with only the two
        // hot-tail columns tumbling in between.
        //
        // NOTE (known limitation, deliberately not "fixed" here).
        //
        // WRITE motion is purely ARRIVAL-driven: the window is anchored on
        // the newest byte. A real SSE wire does not arrive smoothly — a fat
        // delta lands, then several frames pass with nothing — so the tape
        // holds still through the quiet frames and then advances a whole
        // phrase at once. Only the two hot-tail columns tumble in between.
        // That is the "it's not continuous" report, and it is real.
        //
        // A clock-driven smoothing ramp was attempted and REVERTED. Two
        // dead ends, both measured, recorded so they aren't retried blind:
        //
        //   1. anchor = min(now/rate, total) glides only while the ramp
        //      trails the byte total — true near t=0, false in any real
        //      session, where it degenerates to "newest byte" and freezes
        //      exactly as before. (0 frozen frames at t0=0, 7 of 14 at
        //      t0=5000 — a probe that only tried t0=0 called it fixed.)
        //   2. anchor = total - (depth - phase) with phase a modulo of the
        //      clock removes the freezes but makes the window REWIND when
        //      the phase wraps: a sawtooth, i.e. the same teleport class
        //      this file just removed from the READ head. Strictly worse
        //      than stuttering.
        //
        // Doing this properly needs the widget to remember its own last
        // anchor across frames (a monotonic pursuit of the head, never
        // decreasing) rather than deriving position from the clock alone.
        // ActivityIndicator is currently a pure function of (config, clock)
        // and is rebuilt every frame, so that is a real design change, not
        // a tweak — and it must keep the frozen-clock testability that
        // makes this widget verifiable at all.
        const std::size_t write_head = stream_total;


        if (write_mode) {
            // The TRUE cumulative total, deliberately not the glided head.
            // This column is the stream's odometer — a real byte count the
            // user can trust — and it must keep counting at the wire's
            // actual rate even while the WINDOW is still paying out a
            // burst. Only the narrated bytes glide; the number is data.
            std::snprintf(offbuf, sizeof(offbuf), "0x%06llx",
                          static_cast<unsigned long long>(stream_total));
        } else if (read_mode) {
            std::snprintf(offbuf, sizeof(offbuf), "0x%06llx",
                          static_cast<unsigned long long>(read_head));
        } else {
            std::snprintf(offbuf, sizeof(offbuf), "0x%06x", 0u);
        }
        const std::string off_str = offbuf;

        const std::string detail = cfg_.detail;

        return component([=, stream_tail = std::move(stream_tail),
                          context = std::move(context),
                          off_str = off_str](int avail_w, int /*h*/) -> Element {
            using namespace dsl;

            // ── Progressive degradation. We pick the richest variant
            // that still fits in avail_w, then size the byte window
            // to consume the remaining budget. Variants from richest
            // to leanest:
            //
            //   FULL    indent + offset + hex + gutter + detail
            //   NODET   indent + offset + hex + gutter
            //   NOHEX   indent + offset + gutter
            //   NOOFF   indent + gutter
            //
            // The words live in the gutter, so we drop hex BEFORE
            // gutter — readable stream text is the row's whole point
            // and must stay visible at every width the row can render.
            enum Variant { FULL, NODET, NOHEX, NOOFF };

            constexpr int kPerByteHex   = 4;   // "xx " + last has no space, +1 added back
            constexpr int kPerByteAscii = 1;
            const int indent_cost = 2;
            // MEASURED, not byte-counted: off_str is ASCII but `detail`
            // is host text — "⚙ résolution" is 14 bytes, 12 cells; a
            // byte count here over-charges the chrome and starves the
            // byte window.
            const int off_cost    = string_width(off_str);
            const int det_cost    = detail.empty() ? 0
                : string_width(detail) + 5;   // "  ·  " + detail
            // Chrome cost = everything that's NOT the per-byte budget.
            // Every variant includes the two outer `|` pipes of the
            // gutter (2 cols) since we never drop the gutter.
            auto fixed_chrome = [&](Variant v) -> int {
                switch (v) {
                    // FULL: indent + off + 2sp + hex + 2sp + gutter(2) + detail
                    case FULL:  return indent_cost + off_cost + 2 + 2 + 2 + det_cost;
                    // NODET: indent + off + 2sp + hex + 2sp + gutter(2)
                    case NODET: return indent_cost + off_cost + 2 + 2 + 2;
                    // NOHEX: indent + off + 2sp + gutter(2)
                    case NOHEX: return indent_cost + off_cost + 2 + 2;
                    // NOOFF: indent + gutter(2)
                    case NOOFF: return indent_cost + 2;
                }
                return 0;
            };
            // Per-column cost in the byte window. FULL/NODET have
            // both hex (3 cols/byte: "xx ") AND ascii (1 col/byte).
            // NOHEX/NOOFF have just ascii.
            auto per_byte = [&](Variant v) -> int {
                if (v == FULL || v == NODET) return kPerByteHex;  // 3 hex + 1 ascii = 4
                return kPerByteAscii;                              // 1
            };
            // Floor at 8 bytes — enough for a readable text fragment —
            // before degrading to a leaner variant.
            const int floor_bytes = 8;

            auto cols_for = [&](Variant v) -> int {
                const int b = avail_w - fixed_chrome(v);
                return b / per_byte(v);
            };
            Variant variant = FULL;
            for (Variant cand : {FULL, NODET, NOHEX, NOOFF}) {
                if (cols_for(cand) >= floor_bytes) { variant = cand; break; }
                variant = cand;   // keep the leanest even if still tight
            }
            int cols = cols_for(variant);
            if (cols < 1) cols = 1;
            if (cols > 32) cols = 32;
            // Even the leanest variant couldn't reach floor_bytes —
            // we're on a truly tiny terminal. Show what we can; the
            // text will be partially clipped but no row will wrap.

            // Sweeping highlight column — independent of the data, so
            // it visually crosses the tape rather than sliding with
            // it. Reads like a debugger read-head.
            const int sweep_col =
                static_cast<int>((now_ms / kSweepMs) % cols);

            // ── Resolve the byte window: (byte, role) per column.
            // role: 0 = settled/plain, 1 = highlighted word, 2 = churn.
            // Hex + ascii render from this one array so they agree
            // byte-for-byte.
            //
            // Shared helper: find the word containing byte index `at`
            // in `src` (separator = non-printable or space), capped at
            // kMaxWordHl so a JSON blob / base64 run can't flood the
            // row with highlight. Returns [from, to] inclusive, or
            // from > to when `at` sits on a separator.
            constexpr int kMaxWordHl = 12;
            auto word_around = [&](const std::string& src, int at)
                -> std::pair<int, int> {
                auto sep = [&](int i) {
                    const unsigned char b =
                        static_cast<unsigned char>(src[
                            static_cast<std::size_t>(i)]);
                    return b <= 0x20 || b >= 0x7f;
                };
                if (at < 0 || at >= static_cast<int>(src.size()) || sep(at))
                    return {0, -1};
                int from = at, to = at;
                while (from > 0 && !sep(from - 1)) --from;
                while (to + 1 < static_cast<int>(src.size()) && !sep(to + 1))
                    ++to;
                if (to - from + 1 > kMaxWordHl) from = to - kMaxWordHl + 1;
                return {from, to};
            };

            std::vector<std::pair<std::uint8_t, int>> resolved;
            resolved.reserve(static_cast<std::size_t>(cols));

            if (write_mode) {
                // ── WRITE: newest bytes, right-anchored on the write
                // head. Motion comes from DATA ARRIVAL (each new byte
                // shifts the window left), not the wall clock — the
                // tape speed IS the stream speed. The last kHotTail
                // bytes churn (the write head runs hot; a byte settles
                // once newer bytes displace it — lock-in driven by
                // arrival, no per-byte timestamps). The word being
                // written right now carries the highlight. A young
                // stream (< cols bytes) left-pads with noise at
                // pre-first-byte positions — honest (those bytes never
                // existed) — so arrival renders as real data eating the
                // static from the right.
                constexpr int kHotTail = 2;
                const std::int64_t shown_total =
                    static_cast<std::int64_t>(write_head);
                const int nvis = static_cast<int>(
                    std::min<std::size_t>(static_cast<std::size_t>(cols),
                                          stream_tail.size()));
                const int pad = cols - nvis;
                const std::int64_t first_off = shown_total
                    - static_cast<std::int64_t>(nvis);
                // Visible base index into stream_tail.
                const int vbase = static_cast<int>(stream_tail.size()) - nvis;
                // Word at the write head (skip trailing separators).
                int head = static_cast<int>(stream_tail.size()) - 1;
                {
                    auto sep = [&](int i) {
                        const unsigned char b = static_cast<unsigned char>(
                            stream_tail[static_cast<std::size_t>(i)]);
                        return b <= 0x20 || b >= 0x7f;
                    };
                    while (head >= 0 && sep(head)) --head;
                }
                const auto [wfrom, wto] = word_around(stream_tail, head);
                for (int c = 0; c < cols; ++c) {
                    if (c < pad) {
                        const std::int64_t pos =
                            first_off - static_cast<std::int64_t>(pad - c);
                        resolved.emplace_back(static_cast<std::uint8_t>(
                            base_byte(pos) ^ drift_byte(pos)), 0);
                        continue;
                    }
                    const int si = vbase + (c - pad);   // index in stream_tail
                    const std::uint8_t b = static_cast<std::uint8_t>(
                        stream_tail[static_cast<std::size_t>(si)]);
                    if (c - pad >= nvis - kHotTail) {
                        // Write head: churn, seeded by time + true offset.
                        std::uint64_t x = static_cast<std::uint64_t>(now_ms)
                                            * 0x9E3779B97F4A7C15ull
                                        ^ static_cast<std::uint64_t>(
                                              first_off + (c - pad))
                                            * 0xD1B54A32D192ED03ull;
                        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27;
                        resolved.emplace_back(
                            static_cast<std::uint8_t>(x & 0xff), 2);
                    } else if (si >= wfrom && si <= wto) {
                        resolved.emplace_back(b, 1);
                    } else {
                        // Same reasoning as the READ arm: these are REAL
                        // bytes off the wire, so a printable one is text and
                        // should look like text. Rendering it identically to
                        // the noise channel made arriving output read as
                        // static with a single lit word in it.
                        const unsigned char u = b;
                        resolved.emplace_back(
                            b, (u > 0x20 && u < 0x7f)
                                   ? static_cast<std::uint8_t>(3)
                                   : static_cast<std::uint8_t>(0));
                    }
                }
            } else if (read_mode) {
                // ── READ: a head scanning the REAL context bytes at
                // kReadMs per byte, window trailing it. The word under
                // the head lights up as it's passed — the model chewing
                // through the prompt. Offsets loop at the context end
                // (re-reading is what attention does anyway). Positions
                // before the context start (young window at the top of
                // a loop) render as static.
                const int rh = static_cast<int>(read_head);
                const auto [wfrom, wto] = word_around(context, rh);
                for (int c = 0; c < cols; ++c) {
                    // Window shows [rh - cols + 1, rh]; the head is the
                    // rightmost column.
                    const int ci = rh - (cols - 1) + c;
                    if (ci < 0) {
                        // Before the loop start — static.
                        resolved.emplace_back(static_cast<std::uint8_t>(
                            base_byte(ci) ^ drift_byte(ci)), 0);
                        continue;
                    }
                    const std::uint8_t b = static_cast<std::uint8_t>(
                        context[static_cast<std::size_t>(ci)]);
                    // Role 1 is the HEAD word (hottest — what's being read
                    // right now). Role 3 is any other real word already in
                    // the window: still the user's actual prompt, so it
                    // reads as text rather than as noise. Previously only
                    // the head word was ever coloured and every other real
                    // byte rendered identically to the random channel
                    // noise, which made a window full of genuine prompt
                    // text look dead — one lit word adrift in static.
                    std::uint8_t role = 0;
                    if (ci >= wfrom && ci <= wto) {
                        role = 1;
                    } else {
                        const unsigned char u = b;
                        if (u > 0x20 && u < 0x7f) role = 3;
                    }
                    resolved.emplace_back(b, role);
                }
            } else {
                // ── STATIC: channel noise, never dressed as data.
                for (int c = 0; c < cols; ++c) {
                    const std::int64_t pos = scroll + c;
                    resolved.emplace_back(static_cast<std::uint8_t>(
                        base_byte(pos) ^ drift_byte(pos)), 0);
                }
            }

            auto style_for = [&](int c, int role) -> Style {
                if (role == 1) return Style{}.with_fg(highlight).with_bold();
                if (role == 2) return Style{}.with_fg(highlight).with_dim();
                // Role 3 — REAL text that isn't the head word. Undimmed in
                // the foreground colour: clearly readable as content, but a
                // step below the bold highlight so the head still leads the
                // eye. Without this tier every real byte except one word
                // rendered exactly like the noise channel.
                if (role == 3) return Style{}.with_fg(text_fg);
                if (c == sweep_col) return Style{}.with_fg(sweep_fg);
                return Style{}.with_fg(muted).with_dim();
            };

            // ── Hex column.
            std::string hex;
            hex.reserve(static_cast<std::size_t>(cols) * 3);
            std::vector<StyledRun> hex_runs;
            hex_runs.reserve(static_cast<std::size_t>(cols) * 2);
            for (int c = 0; c < cols; ++c) {
                const auto [b, role] = resolved[static_cast<std::size_t>(c)];
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02x", b);
                const std::size_t before = hex.size();
                hex.append(buf, 2);
                hex_runs.push_back(StyledRun{before, 2, style_for(c, role)});
                if (c + 1 < cols) {
                    const std::size_t sp = hex.size();
                    hex.push_back(' ');
                    hex_runs.push_back(StyledRun{
                        sp, 1, Style{}.with_fg(muted).with_dim()});
                }
            }

            // ── ASCII gutter.
            std::string ascii;
            ascii.reserve(static_cast<std::size_t>(cols) + 2);
            std::vector<StyledRun> ascii_runs;
            ascii_runs.reserve(static_cast<std::size_t>(cols) + 2);
            ascii.push_back('|');
            ascii_runs.push_back(StyledRun{0, 1, Style{}.with_fg(muted).with_dim()});
            for (int c = 0; c < cols; ++c) {
                const auto [b, role] = resolved[static_cast<std::size_t>(c)];
                const std::size_t before = ascii.size();
                if (b >= 0x20 && b < 0x7f) ascii.push_back(static_cast<char>(b));
                else                       ascii.push_back('.');
                ascii_runs.push_back(StyledRun{before, 1, style_for(c, role)});
            }
            const std::size_t pipe = ascii.size();
            ascii.push_back('|');
            ascii_runs.push_back(StyledRun{pipe, 1, Style{}.with_fg(muted).with_dim()});

            Element off_e = text(off_str) | fgc(muted) | Italic;
            Element hex_e = Element{TextElement{
                .content = std::move(hex),
                .style   = Style{}.with_fg(muted),
                .runs    = std::move(hex_runs),
            }};
            Element ascii_e = Element{TextElement{
                .content = std::move(ascii),
                .style   = Style{}.with_fg(muted),
                .runs    = std::move(ascii_runs),
            }};

            std::vector<Element> parts;
            parts.reserve(8);
            parts.push_back(text("  "));
            if (variant != NOOFF) {
                parts.push_back(std::move(off_e));
                parts.push_back(text("  "));
            }
            if (variant == FULL || variant == NODET) {
                parts.push_back(std::move(hex_e));
                parts.push_back(text("  "));
            }
            // Gutter is always shown — it carries the word.
            parts.push_back(std::move(ascii_e));
            if (variant == FULL && !detail.empty()) {
                parts.push_back(text("  \xc2\xb7  ") | fgc(muted) | Italic);
                parts.push_back(text(detail) | fgc(muted) | Italic);
            }
            // Exactly one row — no internal leading blank. Hosts that
            // need separation from the element above supply it (the
            // in-Turn placeholder gets it from Turn's under-header
            // blank). Staying single-row makes the placeholder height
            // match the first streamed content slot, so the
            // indicator→content flip is height-seamless.
            return h(std::move(parts)).build();
        });
    }

private:
    Config cfg_;
};

} // namespace maya
