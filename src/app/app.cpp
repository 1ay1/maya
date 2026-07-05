#include "maya/app/app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <thread>   // DSR cursor-position query: brief sleep between polls

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <unistd.h>   // isatty, getpid, fileno
#endif

#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"
#include "maya/platform/select.hpp"
#include "maya/platform/thread.hpp"
#include "maya/terminal/ansi.hpp"

namespace maya::detail {

namespace {
// Opt-in event-loop diagnostics. Set MAYA_IO_LOG=<path> to trace the
// poll/wait/read/render boundary to a file; no-op (one getenv) otherwise.
// Diagnostic scaffolding for the WezTerm-on-Windows "frozen animation"
// investigation — safe to leave compiled in.
inline void io_log(const char* fmt, ...) {
    static const char* path = std::getenv("MAYA_IO_LOG");
    if (!path) return;
    static std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(f, "[%9lld] ", static_cast<long long>(t));
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}
} // namespace

// ============================================================================
// Runtime::create — initialize terminal, event source, canvases
// ============================================================================

auto Runtime::create(RunConfig cfg) -> Result<Runtime> {
    // Move the terminal through the type-state chain: Cooked → Raw → AltScreen
    MAYA_TRY_DECL(auto cooked, Terminal<Cooked>::create());
    MAYA_TRY_DECL(auto raw, std::move(cooked).enable_raw_mode());

    auto input_h  = raw.input_handle();
    auto output_h = raw.output_handle();

    // Install resize signal handler.
    MAYA_TRY_DECL(auto resize_sig, platform::NativeResizeSignal::install());

    // Both Fullscreen and Inline transitions consume the Raw terminal and
    // return a new type-state whose destructor reverses the opt-ins.  This
    // is the entire fault-tolerance story for terminal cleanup — no manual
    // cleanup() function can be forgotten because the type's destructor
    // runs on every exit path, including stack unwinding from exceptions.
    std::optional<Terminal<AltScreen>>  alt_term;
    std::optional<Terminal<InlineMode>> inline_term;

    if (cfg.mode == Mode::Fullscreen) {
        MAYA_TRY_DECL(auto alt, std::move(raw).enter_alt_screen());
        alt_term = std::move(alt);
    } else {
        MAYA_TRY_DECL(auto inl, std::move(raw).enable_inline_mode());
        inline_term = std::move(inl);
    }

    // Create event source — multiplexes terminal input and resize signals.
    // The wake fd/handle is registered later via rt.set_wake_fd() /
    // set_wake_handle() once the BackgroundQueue exists in run<P>().
    auto sig_handle = resize_sig.native_handle();
    platform::NativeEventSource event_source(input_h, sig_handle);

    Runtime rt;

    rt.alt_terminal_    = std::move(alt_term);
    rt.inline_terminal_ = std::move(inline_term);
    rt.output_handle_   = output_h;
    rt.input_handle_    = input_h;
    rt.resize_signal_   = std::move(resize_sig);
    rt.event_source_    = std::move(event_source);
    rt.writer_          = std::make_unique<Writer>(output_h);
    rt.theme_           = cfg.theme;
    // Emit DEC mode 2026 (synchronized output) brackets by DEFAULT, not
    // only when the env-heuristic can fingerprint the terminal. On every
    // terminal that supports the mode they make each frame swap atomically
    // — the single most effective flicker cure, and it works even on
    // terminals the heuristic misses (ssh/tmux passthrough, niche
    // emulators, anything that supports 2026 without leaving an env
    // marker). On terminals that DON'T support it the private-mode
    // set/reset is silently ignored (ECMA-48: unknown DEC private modes
    // are no-ops) — harmless-but-pointless, never corrupting. The only
    // opt-out is an explicit MAYA_NO_SYNC, for the rare emulator that
    // echoes unknown private modes as literal text.
    {
        const char* no_sync = std::getenv("MAYA_NO_SYNC");
        const std::string_view ns = no_sync ? no_sync : "";
        const bool disabled = !ns.empty()
            && ns != "0" && ns != "false" && ns != "no";
        // Emit the wrapper unless explicitly disabled — harmless no-op
        // where unsupported, atomic frames where supported.
        rt.emit_sync_wrapper_ = !disabled;
        // The HONEST support answer, for tick-rate gating. MAYA_NO_SYNC
        // forces it false; otherwise consult the env heuristic so apps
        // on Apple Terminal / ish / unconfigured tmux slow their
        // animation cadence instead of tearing every frame.
        rt.sync_supported_ =
            !disabled && ansi::env_supports_synchronized_output();
    }

    // Set terminal title if provided.
    if (!cfg.title.empty()) {
        auto seq = std::format("\x1b]0;{}\x07", cfg.title);
        (void)platform::io_write_all(output_h, seq);
    }

    // Enable mouse reporting if requested. Use maya's canonical sequence:
    // 1000 (button press/release) + 1002 (button-drag motion) + 1006 (SGR
    // extended coordinates) + 1007 (alt-scroll). This matches what
    // enter_alt_screen() emits and what kitty / xterm / wezterm expect —
    // 1003 (ANY-motion) floods move events and some terminals (kitty)
    // handle it inconsistently. Cached on the Runtime so cleanup()/dtor
    // emit the matching disable on every exit path (the InlineMode /
    // AltScreen destructors restore raw mode + screen but the simple-run
    // inline path does NOT enable mouse on its own).
    rt.mouse_enabled_ = cfg.mouse;
    if (cfg.mouse) {
        static constexpr std::string_view kMouseOn =
            "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
        (void)platform::io_write_all(output_h, kMouseOn);
    }

    // Query initial terminal size.
    if (rt.alt_terminal_) {
        rt.size_ = rt.alt_terminal_->size();
    } else if (rt.inline_terminal_) {
        rt.size_ = rt.inline_terminal_->size();
    }

    rt.render_ctx_.width      = rt.size_.width.raw();
    rt.render_ctx_.height     = rt.size_.height.raw();
    rt.render_ctx_.generation = 0;

    // Pre-reserve layout_nodes_ so the first big-frame render doesn't
    // pay a chain of std::vector reallocs (each doubling means the
    // last realloc copies every node built so far). 1024 nodes covers
    // typical agentty trees (composer + scrollback + a few turns);
    // deeper trees still grow on demand via vector's amortised
    // doubling, but the steady-state working set lives entirely
    // inside this initial capacity. Cost: 1024 * sizeof(LayoutNode)
    // ≈ 184 KB — paid once per Runtime lifetime.
    rt.layout_nodes_.reserve(1024);

    // Schedule hint — see platform/thread.hpp. macOS: QoS user-interactive.
    // Linux/Win32: no-op.
    platform::set_ui_thread_priority();

    // Inline mode: start in the Witness Chain's `Empty` state. The first
    // render's path seeds it to `Fresh` (via `seed()`) and runs compose's
    // case (A) — emit from the cursor's current position via serialize(),
    // growing downward without disturbing host content above.
    //
    // We can't pre-seed `cursor_hidden = true` the way the legacy state
    // could because InlineFrame<Empty> has no state to carry. The cost
    // is that compose emits the hide_cursor escape on its first frame
    // when it would have been a no-op under the legacy seed — a 6-byte
    // wire cost paid exactly once per session. Worth it for the type-
    // state guarantee that the runtime starts from a state the chain
    // can construct, not one synthesized by side-effect.
    (void)rt;   // is_inline()-conditional init no longer needed; the default
                // `InlineFrame<Empty>{}` field initializer covers it.

    // ── Inline mouse anchor (cursor-position query / DSR) ────────────────
    // In inline mode the frame is drawn partway down the terminal, but SGR
    // mouse reports are ABSOLUTE. Ask the terminal where the cursor is right
    // now (= where the first frame's top row will land) so the run loop can
    // translate mouse coordinates into frame-relative space. Done here, once,
    // synchronously: during this exchange we KNOW a Cursor-Position Report is
    // coming, so parsing `CSI <row>;<col> R` is unambiguous (in the normal
    // input stream that form collides with modified-F3, which is why we do
    // NOT route it through the parser). Bounded + best-effort: if the
    // terminal never answers, inline_top_row_ stays 0 → no offset → behavior
    // is identical to before. Only runs for inline + mouse.
    if (cfg.mouse && rt.inline_terminal_) {
        (void)platform::io_write_all(output_h, "\x1b[6n");
        std::string resp;
        // Extract a complete `ESC [ rows ; cols R` from `buf`, returning rows
        // and erasing the matched bytes; 0 if none complete yet.
        auto take_cpr = [](std::string& buf) -> int {
            for (std::size_t i = 0; i + 1 < buf.size(); ++i) {
                if (buf[i] != '\x1b' || buf[i + 1] != '[') continue;
                std::size_t j = i + 2; long row = 0; bool digits = false;
                for (; j < buf.size() && buf[j] >= '0' && buf[j] <= '9'; ++j) {
                    row = row * 10 + (buf[j] - '0'); digits = true;
                }
                if (!digits) continue;                 // not a numeric CSI
                if (j >= buf.size()) return 0;          // incomplete → wait
                if (buf[j] != ';') continue;            // some other CSI
                for (++j; j < buf.size() && buf[j] >= '0' && buf[j] <= '9'; ++j) {}
                if (j >= buf.size()) return 0;          // incomplete → wait
                if (buf[j] != 'R') continue;            // not a CPR
                buf.erase(i, j - i + 1);                // splice the CPR out
                return static_cast<int>(row);
            }
            return 0;
        };
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(150);
        int row = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            auto data = rt.inline_terminal_->read_raw();
            if (data && !data->empty()) {
                resp += *data;
                if ((row = take_cpr(resp)) > 0) break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (row > 0) rt.inline_top_row_ = row;
        // Any bytes that weren't the CPR (e.g. a keypress during the query)
        // go through the parser now and are replayed on the first
        // read_events() so no input is dropped.
        if (!resp.empty())
            for (auto& e : rt.parser_.feed(resp))
                rt.startup_events_.push_back(std::move(e));
    }

    return ok(std::move(rt));
}

// ============================================================================
// Runtime::poll — wait for events on the multiplexed event source
// ============================================================================

auto Runtime::poll(std::chrono::milliseconds timeout) -> Result<PollResult> {
    const auto t0 = std::chrono::steady_clock::now();
    MAYA_TRY_DECL(auto ready, event_source_->wait(timeout));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    io_log("poll  timeout=%4lld waited=%4lld  input=%d resize=%d wake=%d",
           static_cast<long long>(timeout.count()),
           static_cast<long long>(waited),
           ready.input ? 1 : 0, ready.resize ? 1 : 0, ready.wake ? 1 : 0);
    return ok(PollResult{.resize = ready.resize, .input = ready.input, .wake = ready.wake});
}

// ============================================================================
// Runtime::handle_resize — update internal state on terminal resize
// ============================================================================

void Runtime::handle_resize() {
    if (resize_signal_) resize_signal_->drain();

    Size new_size;
    if (alt_terminal_) {
        new_size = alt_terminal_->size();
    } else if (inline_terminal_) {
        new_size = inline_terminal_->size();
    } else {
        return;
    }

    if (new_size != size_) {
        const int prev_w = size_.width.raw();
        size_ = new_size;
        ++resize_generation_;
        render_ctx_.width      = size_.width.raw();
        render_ctx_.height     = size_.height.raw();
        render_ctx_.generation = resize_generation_;

        // Width vs height-only resize have very different costs in
        // inline mode:
        //
        //   • Width change   — the prev-frame cell grid is invalid
        //     (every row's wrap points and column layout shift). The
        //     row-diff can't reuse any of it, so we HardReset: next
        //     render emits \x1b[2J\x1b[3J\x1b[H and repaints fresh.
        //     Destructive to host scrollback, but unavoidable.
        //
        //   • Height-only change — same width means every cached row
        //     is still byte-valid; only the viewport's vertical extent
        //     moved. This is the common case on mobile: the soft
        //     keyboard / dictation field opening or closing changes
        //     the terminal HEIGHT, not its width. A HardReset here
        //     would needlessly wipe the screen and force a full
        //     top-to-bottom repaint (the symptom the user saw on
        //     iPhone). Instead demote to Stale: the next render runs
        //     compose's case-(B) soft redraw — walk the cursor up,
        //     repaint the visible viewport in place, erase below.
        //     No \x1b[2J\x1b[3J\x1b[H, no scrollback wipe, host
        //     content above the viewport is preserved, and the
        //     repaint is bounded to the rows that actually moved.
        //
        // Fullscreen always collapses to Divergent (a width-independent
        // full repaint is cheap there because the alt-screen owns the
        // whole grid and there's no host scrollback to preserve).
        const bool width_changed = (prev_w != size_.width.raw());
        fs_coherence_ = coherent::Divergent{};
        in_coherence_ = std::visit(
            [width_changed](auto&& arm) -> inline_frame::InlineCoherence {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Synced>>) {
                    return width_changed
                        ? inline_frame::InlineCoherence{std::move(arm).demote_to_hard_reset()}
                        : inline_frame::InlineCoherence{std::move(arm).demote_to_stale()};
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Stale>>) {
                    // Already Stale: a width change can no longer reuse
                    // the (zeroed) prev grid, so escalate; a height-only
                    // change stays Stale and rides the same case-(B)
                    // soft redraw.
                    return width_changed
                        ? inline_frame::InlineCoherence{std::move(arm).escalate_to_hard_reset()}
                        : inline_frame::InlineCoherence{std::move(arm)};
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Fresh>>) {
                    // Resize before first render: the wire hasn't seen
                    // anything from us yet. Stay Fresh; the next render's
                    // case (A) paints at the new dimensions from the
                    // cursor's current position without disturbing the
                    // host content above.
                    return std::move(arm);
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::HardReset>>) {
                    return std::move(arm);   // already in hard-reset
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Empty>>) {
                    return std::move(arm);   // nothing on screen yet
                } else {
                    // Sealed: finalize already ran; resize is a no-op.
                    return std::move(arm);
                }
            }, std::move(in_coherence_));
    }
}

// ============================================================================
// Runtime::read_events — read and parse terminal input
// ============================================================================

auto Runtime::read_events() -> Result<std::vector<Event>> {
    std::vector<Event> result;

    // Replay events that arrived during create()'s cursor-position query
    // (a keypress interleaved with the DSR reply) ahead of fresh input.
    if (!startup_events_.empty()) result.swap(startup_events_);

    if (alt_terminal_) {
        MAYA_TRY_DECL(auto data, alt_terminal_->read_raw());
        if (!data.empty()) {
            for (auto& event : parser_.feed(data))
                result.push_back(std::move(event));
        }
    } else if (inline_terminal_) {
        io_log("read_raw begin");
        MAYA_TRY_DECL(auto data, inline_terminal_->read_raw());
        io_log("read_raw end bytes=%zu", data.size());
        if (!data.empty()) {
            for (auto& event : parser_.feed(data))
                result.push_back(std::move(event));
        }
    }
    io_log("read_events -> %zu events", result.size());
    return ok(std::move(result));
}

// ============================================================================
// Runtime::flush_timeouts — flush parser timeout events
// ============================================================================

auto Runtime::flush_timeouts() -> std::vector<Event> {
    std::vector<Event> result;
    for (auto& ev : parser_.flush_timeout())
        result.push_back(std::move(ev));
    return result;
}

// ============================================================================
// Runtime::render — render an element tree to the terminal
// ============================================================================
// Fullscreen: RenderPipeline type-state machine (Idle→Cleared→Painted→Opened→Closed)
// Inline: compose_inline_frame (row-diff, scrollback-preserving)

auto Runtime::render(const Element& root) -> Status {
    io_log("render");
    // MAYA_FRAME_PROF=1 enables per-frame timing output. Writing to a
    // tty-attached stderr while inline mode owns stdout would interleave
    // prof lines with cell bytes — visible garbage in the inline area
    // and a permanently stale prev_cells (the renderer doesn't track
    // foreign writes). Resolution:
    //
    //   - MAYA_FRAME_PROF=/path/to/log     → open that path (append).
    //   - MAYA_FRAME_PROF=1 with stderr already redirected (not a tty)
    //     → keep stderr; the user already pointed it somewhere safe.
    //   - MAYA_FRAME_PROF=1 with stderr on a tty
    //     → open /tmp/maya-frame-prof-<pid>.log instead. Deterministic
    //       path; documented here so devs know where to tail from.
    static FILE* const prof_out = []() -> FILE* {
        const char* env = std::getenv("MAYA_FRAME_PROF");
        if (!env || !*env) return nullptr;
        if (env[0] == '/' || env[0] == '.' || env[0] == '~') {
            FILE* fp = std::fopen(env, "a");
            return fp ? fp : nullptr;
        }
#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
        if (!::isatty(::fileno(stderr))) return stderr;
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/maya-frame-prof-%d.log",
                      static_cast<int>(::getpid()));
        FILE* fp = std::fopen(path, "a");
        return fp ? fp : nullptr;   // suppress if we can't open; corrupting the tty is worse
#else
        return stderr;
#endif
    }();
    static const bool prof = prof_out != nullptr;
    auto t_frame_start = std::chrono::steady_clock::now();
    auto since = [&](auto t0) {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count() / 1000.0;
    };

    const int prev_known_w = size_.width.raw();
    if (prev_known_w <= 0) return ok();

    // Per-frame width reconciliation. `term_h` is re-queried every frame
    // (below) so a missed/coalesced SIGWINCH can't desync the viewport
    // height — but WIDTH only updated in handle_resize(). If a resize
    // event is dropped (kitty/tmux coalescing, a fast drag, a size that
    // differed at launch), the renderer keeps composing at the stale
    // width while the terminal is a different size: every row mis-wraps
    // and the diff's \r / cursor-up math lands on the wrong physical
    // rows — the intermittent flicker / corruption symptom. Treat a
    // per-frame width delta exactly like a resize: update size_, bump
    // the generation, and route the next compose through the
    // width-changed reset path (HardReset inline / Divergent fullscreen)
    // so it repaints clean at the true width. Single TIOCGWINSZ, same
    // ioctl that already backs query_term_rows.
    {
        const auto live = platform::query_terminal_size(output_handle_);
        const int live_w = live.width.raw();
        if (live_w > 0 && live_w != prev_known_w) {
            // Hysteresis: a genuine resize persists; a transient TIOCGWINSZ
            // glitch (kitty's alternating 1-2 col flap) does not. Require the
            // same off-width on two consecutive frames before acting, so a
            // single-frame flap never triggers a resize/repaint storm.
            if (live_w == width_candidate_) {
                handle_resize();
                width_candidate_ = 0;
            } else {
                width_candidate_ = live_w;   // first sighting — wait for confirm
            }
        } else {
            width_candidate_ = 0;            // width matches; clear any candidate
        }
    }
    const int w = size_.width.raw();
    if (w <= 0) return ok();

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = is_inline();
    RenderContextGuard ctx_guard(render_ctx_);

    if (is_inline()) {
        // ── Backpressure via non-blocking writer ───────────────────────
        // Output fd is O_NONBLOCK (set by Writer ctor). On a congested
        // tty the previous frame may have left bytes in the writer's
        // residue buffer. Drain those first; if the wire still won't
        // accept them, defer the new frame entirely — DO NOT compose,
        // because compose_inline_frame would update prev_cells to
        // reflect a frame the wire hasn't received, breaking the diff
        // invariant on the next paint.
        //
        // Unlike the older poll(POLLOUT) skip, this can't run away into
        // a feedback loop: prev_cells never lies, so when residue
        // finally drains the next compose produces the same bounded
        // diff (canvas vs wire) it would have on a fast tty. The cost
        // of a deferred render is one event-loop iteration of delay,
        // not an inflated next-frame cost.
        if (writer_->has_residue()) {
            auto d = writer_->try_drain_residue();
            if (!d) {
                if (d.error().kind == ErrorKind::WouldBlock) {
                    return ok();   // wire still backed up; retry next tick
                }
                // Hard I/O error — toss the residue and demote inline
                // coherence to HardReset so the next render does a
                // full reset.
                writer_->discard_residue();
                in_coherence_ = std::visit(
                    [](auto&& arm) -> inline_frame::InlineCoherence {
                        using T = std::decay_t<decltype(arm)>;
                        if constexpr (std::is_same_v<T,
                                inline_frame::InlineFrame<inline_frame::Synced>>)
                            return std::move(arm).demote_to_hard_reset();
                        else if constexpr (std::is_same_v<T,
                                inline_frame::InlineFrame<inline_frame::Stale>>)
                            return std::move(arm).escalate_to_hard_reset();
                        else
                            return std::move(arm);
                    }, std::move(in_coherence_));
                return d;
            }
        }

        // ── Inline path: Witness Chain dispatch ─────────────────────────
        // std::visit selects the InlineFrame<Tag>::render whose
        // precondition matches the current coherence state. Each arm
        // returns a new InlineCoherence directly — the type system
        // guarantees every legal transition is encoded by the return
        // type, and that the only path into Synced is through a
        // successful commit_to of a witness-verified compose.
        constexpr int kMinCanvasHeight = 500;

        // Reasons to (re)allocate the canvas:
        //   — width changed (terminal resize),
        //   — height below the minimum floor,
        //   — height is now ridiculously oversized vs current content.
        //
        // The last condition is load-bearing for long-session perf.
        // canvas_.clear() does streaming_fill over width * height
        // cells every frame; once a tall transcript bumped the
        // canvas to e.g. 6000 rows, clearing 6000 * 100 cells per
        // frame stays expensive forever — even after trim shrunk
        // the actual content back to a few hundred rows.
        //
        // Shrink target: content + 64 (small headroom so a one-turn
        // append doesn't immediately re-grow). Trigger at 1.5x the
        // target instead of 2x so we reclaim memory more eagerly on
        // tool-heavy sessions where the all-time-peak content (a
        // 500-line write that has since trimmed) leaves the canvas
        // sitting at ~1000 rows even though steady-state needs ~200.
        // 1.5x still avoids resize thrash on normal grow/shrink
        // cycles: a turn that adds ~30 rows on top of a steady 400
        // rows lands at 430, well below the 600 trigger.
        const int prev_content_rows = content_height(canvas_);
        const int shrink_target = std::max(kMinCanvasHeight,
                                           prev_content_rows + 64);
        const bool oversized = canvas_.height() * 2 > shrink_target * 3;

        bool canvas_reallocated = false;
        if (canvas_.width() != w
            || canvas_.height() < kMinCanvasHeight
            || oversized) {
            canvas_.set_style_pool(&pool_);
            const int target_h = oversized
                ? shrink_target
                : std::max(kMinCanvasHeight, canvas_.height());
            canvas_.resize(w, target_h);
            canvas_reallocated = true;
        }
        // Recover from any unmatched push_clip in the previous frame's
        // paint pass (e.g. a paint callback that threw past pop_clip).
        canvas_.reset_clips();

        // ── Bounded clear: preserve the immutable frozen prefix ─────────
        // The compose diff only ever reads/rewrites the last `term_h`
        // rows of the canvas; rows above that overflowed into native
        // scrollback and are immutable on the wire. During steady
        // streaming the FROZEN prefix (settled tool cards, prior turns)
        // is byte-stable frame-to-frame: its hash-keyed entries re-blit
        // identical cells to identical rows. Re-clearing + re-blitting
        // those hundreds/thousands of rows every frame is the dominant
        // cost on a tall transcript (a 3000-row `write` result pins the
        // per-frame render at ~11 ms). We therefore clear only the rows
        // at/below a conservative floor and PRESERVE the prefix, letting
        // render_tree's blit_packed_row_cached skip the memcpy for any
        // frozen row whose canvas cells already match (bulk_eq probe;
        // full blit on any mismatch, so correctness never depends on the
        // preservation being valid).
        //
        // SAFETY GATE. The preservation is only sound when the canvas
        // allocation is UNCHANGED since last frame — a resize
        // (reallocation) fills fresh memory, so there is nothing valid
        // to preserve and we must full-clear. Beyond that, the per-row
        // `bulk_eq` guard inside blit_packed_row_cached is the actual
        // correctness backstop: any frozen row whose canvas cells do
        // NOT already match its cached cells (a layout shift moved the
        // prefix, an inter-card gap changed) falls through to a full
        // blit that overwrites the stale bytes. Rows that no entry
        // repaints at all are only ever the all-blank gaps ABOVE the
        // top frozen card, which never carry content the diff emits.
        // The floor is `prev_content_rows - term_h - margin`: strictly
        // above the viewport, so nothing the diff or the
        // scrollback-prefix compare reads is ever preserved (those
        // readers touch [content_rows - term_h, content_rows) and
        // [0, overflow); the margin keeps the preserved region clear of
        // both by construction).
        const int term_h_now = query_term_rows(output_handle_).value();
        int keep_top = 0;
        // Only preserve when the prior frame ended in the steady Synced
        // state (coherence index 2). Any structural event — a trim /
        // freeze that dropped or reshuffled frozen entries, a
        // scrollback commit, a verify-poison recovery — leaves
        // coherence in a non-Synced arm; in those frames the frozen
        // prefix may have shifted or an entry may have vanished
        // (leaving stale preserved rows that would poison max_y_). A
        // full clear on those frames re-establishes a clean canvas, and
        // the preservation resumes on the next steady frame.
        //
        // EXCEPTION the index check can't see: commit_inline_prefix /
        // commit_inline_overflow shift the shadow while REMAINING
        // Synced. After a large front-trim commit the new tree is N
        // rows shorter; preserving the pre-commit canvas would leave
        // stale rows (including the old composer/status chrome)
        // BELOW the new tree's bottom, inflating content_height() and
        // serializing that chrome into native scrollback — the
        // "whole chrome in the scrollback / rows cut off" corruption.
        // Those paths set canvas_preserve_inhibit_; consume it here
        // (one-shot) and full-clear.
        const bool prev_synced = (in_coherence_.index() == 2)
                              && !canvas_preserve_inhibit_;
        canvas_preserve_inhibit_ = false;
        if (!canvas_reallocated
            && prev_synced
            && prev_content_rows > 0
            && term_h_now > 0
            && prev_content_rows > term_h_now) {
            constexpr int kPreserveMargin = 8;
            keep_top = prev_content_rows - term_h_now - kPreserveMargin;
            if (keep_top < 0) keep_top = 0;
        }
        if (keep_top > 0) canvas_.clear_below(keep_top);
        else              canvas_.clear();

        auto t_rt0 = std::chrono::steady_clock::now();
        render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                    /*auto_height=*/true);
        double rt_ms = since(t_rt0);

        int ch = content_height(canvas_);
        // Regrow gate keys on the LAYOUT's computed height, NOT on
        // content_height (max painted row). The two diverge exactly when
        // the rows at the canvas boundary are blank (markdown paragraph
        // separators, turn gaps, card padding): the painted-row proxy
        // stays below canvas.height() while the layout needs more rows,
        // so a `ch >= height` precondition never trips and everything
        // past the canvas bottom — composer + status bar, the LAST
        // children of the frame — is silently clipped until a painted
        // row happens to land on the boundary (the "hidden chrome" bug).
        if (!layout_nodes_.empty()) {
            int needed = layout_nodes_[0].computed.size.height.raw();
            if (needed > canvas_.height()) {
                // Grow with HEADROOM (~25%, min 64 rows) rather than a
                // bare +8. A streaming turn's live tail gains rows every
                // frame; with +8 the very next frame's content again
                // exceeded canvas height, so this branch fired a SECOND
                // full render_tree pass on EVERY growing frame (double the
                // layout+paint cost for the whole stream once a thread has
                // passed the kMinCanvasHeight floor). A generous slack lets
                // many frames of growth land in one allocation, so the
                // re-render fires once every ~N frames instead of always.
                // Bounded under the oversized-shrink trigger (1.5x) above
                // so it can never thrash against the shrink path.
                const int headroom = std::max(64, needed / 4);
                canvas_.resize(w, needed + headroom);
                canvas_.clear();
                render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                            /*auto_height=*/true);
                ch = content_height(canvas_);
            }
        }

        if (ch <= 0) {
            // Empty frame — leave coherence as-is; the wire is unchanged.
            return ok();
        }

        // ── Transient monotonic-height hold (composer anti-bounce) ──
        // The inline composer rides content_height, so a 1-row dip in the
        // live transcript bounces it up then back down. The dips are
        // artefacts of the live tree mutating (activity indicator handing
        // off to the first revealed char — a different subtree; the
        // typewriter crossing a block boundary; a tool card collapsing).
        // None are a height change the user should perceive.
        //
        // maya absorbs them AUTONOMOUSLY — no host policy bit, so there is
        // no Cmd-delivery race against the render that shows the dip
        // (an earlier host-driven design lost that race: the bit arrived
        // long after the dip window had passed). The rule is purely local
        // and conservative:
        //   • Only while content FITS the viewport (unpadded <= term_h):
        //     once it overflows, the composer is pinned at the viewport
        //     bottom and a dip can't move it, so the hold disengages.
        //   • Track a running-max `hold_peak_`. When unpadded content dips
        //     below the peak, pad up to the peak so the rendered height
        //     stays put. The peak rises instantly with content.
        //   • DECAY: a dip is only ever transient IF content is on its
        //     way back up. A streaming markdown reveal sits BELOW peak for
        //     many frames while it slowly re-grows (the typewriter reveals
        //     row by row, the StreamingMarkdown widget's height is
        //     non-monotone across block boundaries) — that must stay
        //     bridged the whole time or the composer bounces mid-reveal.
        //     So decay only advances on frames where content is NOT
        //     climbing (this frame <= last frame): a genuine settle/fold
        //     STAYS flat or shrinks, whereas a reveal keeps rising and
        //     resets the counter. Once the lower height has been stable
        //     (non-rising) for kHoldDecayFrames, the shrink is real and we
        //     let the peak fall to it (pad → 0) so idle carries no dead
        //     space. This is a brief bridge across a transient dip, never
        //     a permanent floor.
        {
            // The hold bridges a SMALL transient seam dip only (the
            // indicator→first-text handoff is 1-2 rows). A larger dip is
            // never a seam artefact — it is real content movement (a
            // markdown block committing/expanding, a fold toggle, the
            // reveal clip crossing a block) and MUST show immediately,
            // not be padded then snapped. So the pad is capped: any dip
            // beyond kMaxHoldPad is shown as-is (peak follows content
            // down at once). Without this cap the hold built up 30-40
            // blank rows on a long reveal, then released them in one
            // frame — the "goes up and redraws everything suddenly" jump.
            constexpr int kMaxHoldPad = 2;
            const int prev_pad = render_ctx_.inline_min_content;
            const int unpadded = ch - prev_pad;          // real content this frame
            // Fool-proof scrollback invariant: the pad must be ZERO at
            // the moment content crosses the viewport boundary into
            // native scrollback, so a height change from the pad can
            // never perturb the overflow→scrollback seam. We therefore
            // only ever engage the hold while content sits with at least
            // kMaxHoldPad rows of headroom BELOW the viewport bottom
            // (the `band` ceiling). Within that last band — content
            // about to overflow — the pad is forced to zero, so the
            // crossing always happens at pad=0. Combined with the
            // per-dip cap (pad <= kMaxHoldPad), the pad is structurally
            // incapable of being present when rows cross term_h.
            const int band = size_.height.raw() - kMaxHoldPad;
            int new_pad = 0;
            if (unpadded <= band) {
                if (unpadded >= hold_peak_) {
                    // Content caught up to / passed the peak: track it,
                    // reset the decay counter, no pad needed.
                    hold_peak_       = unpadded;
                    hold_decay_      = 0;
                } else if (hold_peak_ - unpadded > kMaxHoldPad) {
                    // Dip too large to be a seam transient — it's real
                    // content movement. Drop the peak to it instantly so
                    // the height shows the true content; no pad, no snap.
                    hold_peak_  = unpadded;
                    hold_decay_ = 0;
                } else {
                    // Small dip (<= kMaxHoldPad) — the seam transient the
                    // hold exists for. Bridge it. Only count down toward
                    // releasing when content has STOPPED climbing back.
                    if (unpadded <= hold_last_unpadded_) {
                        if (++hold_decay_ >= kHoldDecayFrames) {
                            hold_peak_  = unpadded;
                            hold_decay_ = 0;
                        } else {
                            new_pad = hold_peak_ - unpadded;
                        }
                    } else {
                        // Still climbing back toward the peak — keep the
                        // bridge and reset decay.
                        hold_decay_ = 0;
                        new_pad     = hold_peak_ - unpadded;
                    }
                }
            } else {
                // At/above the headroom band (content near or past the
                // viewport bottom) — disengage and reset so the pad is
                // zero across the overflow seam and a fresh peak starts
                // the next time content settles back into the band.
                hold_peak_  = 0;
                hold_decay_ = 0;
            }
            hold_last_unpadded_ = unpadded;
            // If the pad changed, the tree we just laid out is stale by
            // `new_pad - prev_pad` rows. Re-run layout+paint ONCE so the
            // committed frame already carries the corrected pad — no
            // one-frame flash of the unpadded height ever reaches the
            // wire. The pad rows are emitted by a LAZY component in
            // AppLayout::build (reads inline_min_content at paint time, as
            // the vstack's last child), so this re-render picks up the new
            // value. Cheap: live tail ~1 viewport, frozen prefix
            // cache-blitted; fires only on a dip/decay frame, never in
            // steady streaming.
            if (new_pad != prev_pad) {
                render_ctx_.inline_min_content = new_pad;
                canvas_.reset_clips();
                canvas_.clear();
                render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                            /*auto_height=*/true);
                ch = content_height(canvas_);
            }
        }

        // Typed terminal-rows witness. Re-queried via
        // `query_term_rows(output_handle_)` per render so a resize
        // between the layout pass and the compose can't desync the
        // viewport bounds compose uses to decide what scrolls off
        // (the case-(B) erase distance + the will_scroll_off
        // heuristic both consume this value). Cheap — a single
        // TIOCGWINSZ ioctl on POSIX.
        const TermRows term_h = query_term_rows(output_handle_);
        const auto rows   = content_rows(canvas_);   // typed witness
        auto t_cf0 = std::chrono::steady_clock::now();

        // Maintain the inline mouse anchor: if the frame no longer fits below
        // its recorded top row, the terminal scrolled it up to make room, so
        // move the anchor up by the overflow. Keeps mouse-row translation
        // correct as content grows. No-op unless we have an anchor (inline +
        // mouse, DSR answered).
        if (inline_top_row_ > 0) {
            const int h = rows.value();
            inline_frame_rows_ = h;   // for out-of-frame mouse suppression
            if (inline_top_row_ + h - 1 > term_h.value())
                inline_top_row_ = std::max(1, term_h.value() - h + 1);
        }

        // Coherence state index before the visit, so the prof log can
        // flag any frame that DIDN'T stay Synced→Synced — those are the
        // full-viewport repaints (Stale soft-redraw, HardReset wipe,
        // verify-poison demote) that show as intermittent flicker.
        const std::size_t coh_before = in_coherence_.index();
        bool verify_demoted = false;

        in_coherence_ = std::visit(
            [&](auto&& arm) -> inline_frame::InlineCoherence {
                using T = std::decay_t<decltype(arm)>;
                using namespace inline_frame;

                auto lift = [](auto&& outcome) -> InlineCoherence {
                    return std::visit(
                        [](auto&& a) -> InlineCoherence { return std::move(a); },
                        std::move(outcome));
                };

                if constexpr (std::is_same_v<T, InlineFrame<Empty>>) {
                    auto fresh = std::move(arm).seed();
                    return lift(std::move(fresh).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        emit_sync_wrapper_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        emit_sync_wrapper_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                    // ── THE SCROLLBACK INVARIANT (one gate) ──────────
                    //
                    // A row that has scrolled into native terminal
                    // scrollback is IMMUTABLE — we can never rewrite it.
                    // The per-row diff assumes the committed overflow
                    // prefix (rows [0, prev_rows-term_h) of the previous
                    // frame) is still byte-identical in the new canvas.
                    // agentty keeps a TALL live tail during streaming, so
                    // that assumption breaks whenever the content above
                    // the viewport top SHIFTS between frames:
                    //
                    //   • a card SHRINKS at/above the viewport top (turn
                    //     settle, code-block fold) → rows below shift UP;
                    //   • a card GROWS mid-turn (a bash tool going
                    //     Running→Failed swaps a 1-row spinner for a
                    //     multi-line error body) → rows above shift UP and
                    //     a content-changed row (e.g. an ACTIONS header
                    //     "0/1 … Bash" → "1/1 … 114ms") crosses the top;
                    //   • the shadow gets poisoned mid-scroll.
                    //
                    // verify() CANNOT catch any of these — the shadow is
                    // internally consistent; it is the WIRE that now holds
                    // stale committed rows. A naive diff re-emits the
                    // shifted prefix over immutable scrollback, stranding
                    // a duplicate one screen up (the reported "card cut
                    // off / turn duplicated in scrollback" corruption).
                    //
                    // agent_session never trips this: its live tail
                    // collapses to empty at MessageStop (frozen and live
                    // are mutually exclusive) so its frames stay
                    // viewport-sized and nothing above the top ever
                    // shifts. agentty's two-tier render can't guarantee
                    // that structurally, so we enforce the invariant HERE,
                    // uniformly, with ONE gate that every overflowed frame
                    // passes through BEFORE the diff is trusted:
                    //
                    //   overflowed AND committed-prefix MISMATCH ?
                    //     → the diff is unsafe. Recover — GROW and SHRINK
                    //       alike — by committing the off-viewport rows and
                    //       soft-repainting the viewport via case (B).
                    //       Non-destructive, host scrollback preserved (no
                    //       \x1b[3J).
                    //
                    //       HISTORY: the growing arm used to demote to
                    //       HardReset (\x1b[2J\x1b[3J\x1b[H wipe) because
                    //       the case-(B) of that era serialized from canvas
                    //       row 0 with bottom-edge scrolls — a grown frame
                    //       re-overflowed on repaint and scrolled a SECOND
                    //       copy into scrollback (maya f010530). Today's
                    //       case-(B) is VIEWPORT-CAPPED (start_row =
                    //       content_rows - term_h, cursor_up ≤ term_h - 1,
                    //       exactly term_h rows with term_h - 1 inter-row
                    //       \r\n — see serialize.cpp's EMIT SHAPE
                    //       contract): zero bottom-edge scrolls at ANY
                    //       content height, so the second-copy failure mode
                    //       is structurally impossible and the wipe is pure
                    //       loss. The wipe erased the user's ENTIRE
                    //       transcript + pre-launch shell history whenever
                    //       a rare untested-regime shift landed on a grow
                    //       frame ("only the last few turns are visible").
                    //
                    //       Residual cost of the soft recovery: the rows
                    //       already in native scrollback keep their
                    //       PRE-shift bytes (immutable at the VT level —
                    //       nothing could have fixed them short of the
                    //       wipe), and the grow delta's rows never reach
                    //       scrollback (a gap of new_rows - prev_rows rows
                    //       at the seam). Bounded, cosmetic, and strictly
                    //       better than losing the whole history.
                    //   overflowed AND prefix MATCHES ? → the diff is safe
                    //     (append-only for grow-below-the-top, shrink at
                    //     the bottom). Fall through to verify()+render.
                    //   not overflowed ? → nothing is committed; the diff
                    //     can rewrite every on-screen row. Fall through.
                    //
                    // This single check SUBSUMES the former three separate
                    // shrink / grow / verify-poison-overflow branches: any
                    // overflowed-prefix shift, from ANY cause (present or
                    // future), is caught by the one memcmp before the diff
                    // ever runs. Corruption is prevented by construction,
                    // not by enumerating the ways a card can move.
                    {
                        const int prev_rows = arm.rows();
                        const int new_rows  = rows.value();
                        if (prev_rows > term_h.value()) {
                            const int overflow = prev_rows - term_h.value();
                            if (!arm.scrollback_prefix_matches(canvas_,
                                                               overflow)) {
                                verify_demoted = true;
                                // Commit + soft-repaint — for GROW and
                                // SHRINK alike. Case (B) is viewport-
                                // capped (no bottom-edge scroll at any
                                // content height), so the grown frame
                                // repaints in place without stranding a
                                // second copy; see the recovery rationale
                                // in the invariant comment above.
                                (void)new_rows;
                                auto marker = arm.scrollback_marker(overflow);
                                auto committed = std::move(arm).commit(marker);
                                return std::move(committed).demote_to_stale();
                            }
                            // prefix MATCHES — the committed rows are
                            // byte-identical to what physically overflowed;
                            // the diff is scrollback-safe. Fall through.
                        }
                    }
                    auto wit = arm.verify();
                    if (!wit) {
                        verify_demoted = true;
                        // Shadow poisoned: prev_cells no longer matches the
                        // wire. If the frame is OVERFLOWED, the committed
                        // prefix was already validated by the invariant
                        // gate above (it matched, else we'd have recovered),
                        // so a plain commit-off-viewport + soft-repaint is
                        // guaranteed safe (the rows we commit equal what
                        // physically overflowed). If it FITS the viewport,
                        // no row scrolled off — every diverged row is still
                        // on screen and rewritable, so a plain demote-to-
                        // Stale (case B) repaints in place. Either way,
                        // NON-destructive: no \x1b[3J wipe.
                        const int prev_rows = arm.rows();
                        if (prev_rows > term_h.value()) {
                            const int overflow = prev_rows - term_h.value();
                            auto marker = arm.scrollback_marker(overflow);
                            auto committed = std::move(arm).commit(marker);
                            return std::move(committed).demote_to_stale();
                        }
                        return std::move(arm).demote_to_stale();
                    }
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        *std::move(wit), emit_sync_wrapper_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        emit_sync_wrapper_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        emit_sync_wrapper_));
                }
                else {
                    static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                    return std::move(arm);   // sealed: no-op
                }
            }, std::move(in_coherence_));

        double cf_ms = since(t_cf0);
        if (prof) {
            // coh: which inline coherence arm RAN this frame (the
            // before-index). 2 == Synced (the steady, no-flicker path);
            // anything else is a full-viewport repaint. `demote` flags
            // the verify-poison Synced→Stale transition specifically.
            std::fprintf(prof_out,
                "maya-frame: rt=%.2f cf=%.2f total=%.2f nodes=%zu rows=%d w=%d "
                "term_h=%d coh=%zu->%zu peak=%d pad=%d decay=%d%s%s\n",
                rt_ms, cf_ms, since(t_frame_start),
                layout_nodes_.size(), ch, w, term_h.value(),
                coh_before, in_coherence_.index(),
                hold_peak_, render_ctx_.inline_min_content, hold_decay_,
                coh_before != 2 ? " FLICKER" : "",
                verify_demoted ? " VERIFY-DEMOTE" : "");
            std::fflush(prof_out);
        }
        return ok();
    }

    // ── Fullscreen path: dispatch on coherence variant ──────────────────
    // The front canvas lives only inside FullscreenSynced.  The diff
    // pipeline can only be invoked from inside that lambda — the
    // serialize path is the sole option for Divergent.  std::visit
    // statically rejects any future state we forget to handle.
    const int h = size_.height.raw();
    if (h <= 0) return ok();

    if (canvas_.width() != w || canvas_.height() != h) {
        canvas_.set_style_pool(&pool_);
        canvas_.resize(w, h);
        // The front (if any) was sized for the old terminal — drop it.
        if (std::holds_alternative<coherent::FullscreenSynced>(fs_coherence_))
            fs_coherence_ = coherent::Divergent{};
    }

    // ── Backpressure via non-blocking writer (parity with inline) ──────
    // Output fd is O_NONBLOCK. If the previous frame couldn't be fully
    // drained, its tail sits in the writer's residue. Drain it before
    // composing a new frame; if the wire still won't accept, defer this
    // render entirely — DO NOT compose, because front-canvas diffing
    // would update `front` to reflect bytes the wire hasn't received,
    // breaking the diff invariant. `has_pending_writes()` keeps the
    // outer event loop spinning at ~8 ms intervals until residue clears
    // (see app.hpp:1058 and 1266). Before this change the fullscreen
    // path used `write_raw` and propagated WouldBlock as a hard error,
    // tearing down the program on the first frame that exceeded the
    // tty buffer (common: doom-fire style heavy first frames in a
    // small pty).
    if (writer_->has_residue()) {
        auto d = writer_->try_drain_residue();
        if (!d) {
            if (d.error().kind == ErrorKind::WouldBlock) {
                return ok();   // wire still backed up; retry next tick
            }
            // Hard I/O error — toss residue and demote to Divergent so
            // the next successful render does a full serialize.
            writer_->discard_residue();
            fs_coherence_ = coherent::Divergent{};
            return d;
        }
    }

    Status write_status = ok();
    fs_coherence_ = std::visit(overload{
        // Synced → Synced (success) or Synced → Divergent (write fail).
        [&](coherent::FullscreenSynced& s) -> coherent::FullscreenState {
            out_.clear();
            auto opened = RenderPipeline<stage::Idle>::start(canvas_, pool_, theme_, out_)
                .clear()
                .paint(root, layout_nodes_)
                .open_frame(emit_sync_wrapper_);
            std::move(opened).write_diff(s.front).close_frame(emit_sync_wrapper_);
            // write_or_buffer: ships what fits, stashes the rest in
            // residue. The residue is drained at the top of the next
            // render (above); until then `has_residue()` keeps the
            // event loop polling at 8 ms. Hard I/O errors still demote
            // to Divergent — discard_residue() because the buffered
            // tail is from a frame we're about to abandon.
            if (auto wr = writer_->write_or_buffer(out_); !wr) {
                writer_->discard_residue();
                write_status = wr;
                return coherent::Divergent{};       // drop the stale front
            }
            // Front ↔ back swap.  The just-composed canvas content is
            // now the canonical front (the bytes are either on the
            // wire or safely in residue, which drains before next
            // compose); the old front becomes the recyclable back
            // buffer for the next paint.
            std::swap(s.front, canvas_);
            canvas_.reset_damage();
            return coherent::FullscreenSynced{std::move(s.front)};
        },

        // Divergent → Synced (success) or Divergent → Divergent (write fail).
        [&](coherent::Divergent) -> coherent::FullscreenState {
            out_.clear();
            auto opened = RenderPipeline<stage::Idle>::start(canvas_, pool_, theme_, out_)
                .clear()
                .paint(root, layout_nodes_)
                .open_frame(emit_sync_wrapper_);
            // Home + serialize every row (no \x1b[2J — flashes inside DEC
            // sync). serialize() appends \x1b[K per row to wipe stale
            // trailing content.
            out_ += "\x1b[H";
            serialize(canvas_, pool_, out_);
            std::move(opened).close_frame(emit_sync_wrapper_);
            if (auto wr = writer_->write_or_buffer(out_); !wr) {
                writer_->discard_residue();
                write_status = wr;
                return coherent::Divergent{};
            }
            // canvas_ now mirrors what's on the terminal (or will, once
            // residue drains) — promote it to the new front. Allocate
            // a fresh back of matching size for the next paint cycle.
            Canvas new_back(canvas_.width(), canvas_.height(), &pool_);
            Canvas new_front = std::exchange(canvas_, std::move(new_back));
            canvas_.reset_damage();
            return coherent::FullscreenSynced{std::move(new_front)};
        },
    }, fs_coherence_);
    return write_status;
}

// ============================================================================
// Runtime::warmup_render — pre-populate cross-frame component cache
// ============================================================================
//
// Hot path on resume of a heavy thread: the FIRST render() pays full
// layout + paint over the rehydrated frozen tree (tens to hundreds of
// ms for tool-heavy threads). The cells are then captured into the
// renderer's hash-keyed ComponentCache and every subsequent frame is
// a memcpy-blit per cached entry (sub-millisecond).
//
// warmup_render() is the same render_tree() call, into a private
// canvas, NOT touching writer_ / coherence state. The cache state it
// leaves behind is what makes the next real render() take the fast
// path.
//
// Width/height: matches the live canvas_'s width (cached cells are
// width-keyed and a width mismatch invalidates the entry); height
// gets `auto_height=true` and grows under content.
void Runtime::warmup_render(const Element& root) {
    const int w = canvas_.width();
    if (w <= 0) return;   // pre-create state; render() will populate later.

    // Scratch canvas — same pool as the live render so captured style
    // ids stay valid when blit'd. Seed height at the current live
    // canvas height so we usually avoid the grow-and-retry below.
    const int seed_h = std::max(64, canvas_.height());
    Canvas scratch(w, seed_h, &pool_);
    scratch.clear();

    std::vector<layout::LayoutNode> nodes;
    nodes.reserve(layout_nodes_.size() + 64);
    render_tree(root, scratch, pool_, theme_, nodes, /*auto_height=*/true);

    // If content overflowed the seed height, grow once and re-render
    // — mirrors the live render() path so the cache entries that get
    // captured are at the same width/height as the next live frame.
    // Same layout-height regrow gate as the live render() path (see the
    // "hidden chrome" rationale there): content_height under-reports when
    // the boundary rows are blank, so key on the layout's needed height.
    if (!nodes.empty()) {
        int needed = nodes[0].computed.size.height.raw();
        if (needed > scratch.height()) {
            scratch.resize(w, needed + 8);
            scratch.clear();
            render_tree(root, scratch, pool_, theme_, nodes,
                        /*auto_height=*/true);
        }
    }
    // scratch is dropped here; the side effect we wanted — the
    // thread_local component_cache holding cells keyed by every
    // hash_id under `root` — persists to the next render() call.
}

// ============================================================================
// Runtime::set_title — set terminal title via OSC 0
// ============================================================================
//
// Routed through write_or_buffer so the OSC queues behind any pending
// render residue. write_raw bypassed the residue queue and called
// write_all directly, which on a partial-write WouldBlock emits the
// CAN/SUB/ST recovery sequence — landing mid-frame on the wire while
// a streaming compose's bytes are still draining. The interleave
// could split a CSI mid-sequence, corrupting the wire's contents
// against our prev_cells shadow and freezing the visible frame until
// a hard redraw (resize) rebuilt shadow from scratch. Same fix shape
// as write_clipboard's earlier migration; the two OSC paths now have
// identical residue semantics.
void Runtime::set_title(std::string_view title) {
    auto seq = std::format("\x1b]0;{}\x07", title);
    (void)writer_->write_or_buffer(seq);
}

// ============================================================================
// Runtime::write_clipboard — system clipboard via OSC 52
// ============================================================================
//
// OSC 52 protocol:
//
//   ESC ] 52 ; c ; <base64-encoded-utf8> ST
//
// The `c` selector targets the regular clipboard (vs `p` for primary on
// X11). Bytes are base64-encoded so binary / multi-line payloads survive
// the terminal's escape-sequence parser unaltered. Empty payload clears
// the clipboard — routed through the encoder uniformly, no special-case.
//
// Cross-platform story:
//
//   Transport: writer_ → platform::io_write, which dispatches to
//   POSIX `::write()` on Linux/macOS and Win32 `::WriteFile()` on
//   Windows. The platform abstraction is the same one set_title()
//   uses and matches every other byte that leaves the runtime.
//
//   Protocol: OSC 52 is interpreted by the terminal emulator, not
//   the OS. Native support: xterm, kitty, alacritty, wezterm, foot,
//   ghostty, rio, iTerm2 (opt-in: Prefs → General → Selection →
//   "Applications in terminal may access clipboard"), Windows
//   Terminal 1.7+, tmux/screen with set-clipboard enabled.
//   Terminals without OSC 52 support discard the OSC string
//   silently per ECMA-48 §8.3.89.
//
// Bytes go through write_or_buffer (not write_raw) so the sequence
// queues behind any pending render residue instead of landing
// mid-frame on the wire — interleaving OSC 52 bytes into an
// unfinished CSI / OSC from the previous compose corrupts the wire's
// shadow against prev_cells and causes a visible repaint glitch.
//
// Terminator: ST (ESC \) rather than BEL. ST is the spec terminator
// per ECMA-48 §8.3.143; BEL is the historic xterm shorthand. ST
// survives tmux's set-clipboard passthrough cleanly on every tmux
// version we've tested; BEL is mangled by some older builds.
void Runtime::write_clipboard(std::string_view text) {
    // RFC 4648 standard alphabet — OSC 52 requires standard (not URL-safe)
    // base64. Padding is required per the OSC 52 spec; terminals reject
    // non-padded payloads inconsistently.
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve((text.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* src = reinterpret_cast<const unsigned char*>(text.data());
    while (i + 3 <= text.size()) {
        std::uint32_t v = (std::uint32_t(src[i])     << 16)
                        | (std::uint32_t(src[i + 1]) <<  8)
                        |  std::uint32_t(src[i + 2]);
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back(kB64[(v >>  6) & 0x3F]);
        encoded.push_back(kB64[ v        & 0x3F]);
        i += 3;
    }
    const std::size_t rem = text.size() - i;
    if (rem == 1) {
        std::uint32_t v = std::uint32_t(src[i]) << 16;
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (rem == 2) {
        std::uint32_t v = (std::uint32_t(src[i])     << 16)
                        | (std::uint32_t(src[i + 1]) <<  8);
        encoded.push_back(kB64[(v >> 18) & 0x3F]);
        encoded.push_back(kB64[(v >> 12) & 0x3F]);
        encoded.push_back(kB64[(v >>  6) & 0x3F]);
        encoded.push_back('=');
    }

    auto seq = std::format("\x1b]52;c;{}\x1b\\", encoded);
    (void)writer_->write_or_buffer(seq);
}

void Runtime::query_clipboard() {
    // OSC 52 read query. The reply (the terminal's clipboard, base64'd)
    // arrives on the input stream and is decoded by InputParser into a
    // PasteEvent. write_or_buffer so a congested tty doesn't drop it;
    // it's a control sequence the diff path never re-emits.
    (void)writer_->write_or_buffer(ansi::request_clipboard());
}

// ============================================================================
// Runtime::suspend — hand the real terminal to an interactive child
// ============================================================================
//
// Blocks on the UI thread for the child's whole run — deliberate: the
// user is interacting WITH the child (sudo password, live output), so
// there is nothing for the UI thread to do meanwhile. The heavy lifting
// (mode teardown/restore escapes, cooked↔raw toggling on the same fd)
// lives in Terminal<State>::suspend so the escape sets stay adjacent to
// the destructor sequences they mirror.
//
// Post-suspend repaint policy:
//   • Fullscreen → Divergent: alt-screen re-entry cleared the buffer,
//     the next render must full-serialize from home. force_redraw()
//     already encodes exactly that.
//   • Inline → the child scrolled arbitrary content under our frame;
//     the physical viewport no longer matches prev_cells anywhere. The
//     inline coherence must drop to a state that repaints from the
//     cursor's CURRENT position without trusting any prior row
//     accounting. reset_to_fresh_after_suspend(): re-anchor like a
//     first render (case A — serialize at cursor, grow downward,
//     never touch the child's output above). The child's output thus
//     stays in native scrollback ABOVE the re-rendered frame, exactly
//     like shell history above a freshly-launched TUI.
//
// Mouse reporting is torn down/restored by the Terminal suspend only in
// alt mode; inline mode enables mouse at the Runtime layer (create), so
// mirror that here.
void Runtime::suspend(const std::function<void()>& fn) {
    if (!fn) return;
    static constexpr std::string_view kMouseOn  = "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
    static constexpr std::string_view kMouseOff = "\x1b[?1007l\x1b[?1006l\x1b[?1002l\x1b[?1000l";

    // Flush any residue the writer is still holding — those bytes belong
    // to the pre-suspend frame and must not interleave with the child's
    // output after the mode switch. Bounded best-effort drain (~200 ms):
    // the fd is non-blocking, so spin try_drain_residue with tiny sleeps
    // rather than blocking forever on a wedged tty.
    if (writer_ && writer_->has_residue()) {
        for (int i = 0; i < 100 && writer_->has_residue(); ++i) {
            if (auto st = writer_->try_drain_residue(); !st) break;
            if (writer_->has_residue())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (mouse_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, kMouseOff);

    // Drop O_NONBLOCK for the child — it shares the open file
    // description and pagers / bulk writers don't expect EAGAIN.
    if (writer_) writer_->suspend_nonblocking();

    if (alt_terminal_) {
        (void)alt_terminal_->suspend(fn);
    } else if (inline_terminal_) {
        (void)inline_terminal_->suspend(fn);
    } else {
        fn();
    }

    if (writer_) writer_->resume_nonblocking();

    if (mouse_enabled_ && output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, kMouseOn);

    // ── Re-anchor rendering ──
    fs_coherence_ = coherent::Divergent{};
    if (inline_terminal_) {
        // The child may have scrolled/printed anything; no prior inline
        // state is trustworthy. Re-seed to Empty — the next render takes
        // the first-frame path (case A): serialize at the cursor's
        // current row, growing downward, leaving the child's output
        // intact above. This is the same safe initial state create()
        // uses, for the same reason.
        in_coherence_ = inline_frame::InlineFrame<inline_frame::Empty>{};
        // The frame anchor learned at create() is stale — the child
        // moved the cursor arbitrarily. Zero it so mouse translation
        // doesn't mis-map rows until the next DSR/anchor pass.
        inline_top_row_    = 0;
        inline_frame_rows_ = 0;
    }
}

// ============================================================================
// Runtime::cleanup — final terminal cleanup
// ============================================================================

auto Runtime::cleanup() -> Status {
    // Disable mouse reporting if create() turned it on. The InlineMode /
    // AltScreen terminal destructors restore raw mode + screen state but
    // know nothing about mouse tracking, so without this the terminal is
    // left echoing SGR mouse reports (\x1b[<…M) as literal text into the
    // user's shell after the app exits. output_handle_ is still valid here
    // (cleanup runs before ~Runtime / the terminal destructors). Idempotent.
    if (mouse_enabled_) {
        static constexpr std::string_view kMouseOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_, kMouseOff);
        mouse_enabled_ = false;
    }
    // Both terminal states (Terminal<AltScreen>, Terminal<Inline>) reverse
    // their own opt-ins in their destructors, so the rest of cleanup is
    // structurally guaranteed by the type system — there is no path where
    // ~Runtime runs without the terminal being restored. This method is
    // kept for ABI/API stability with pre-type-state callers that still
    // invoke (void)rt.cleanup().
    return ok();
}

// ============================================================================
// Runtime move constructor
// ============================================================================

Runtime::~Runtime() {
    // Guaranteed mouse-off on EVERY exit path, including stack unwinding
    // from an exception thrown inside a render/event callback (the simple
    // run() loops call cleanup() only on the normal path; a throwing
    // callback would skip it). Idempotent with cleanup() via the flag.
    // Runs BEFORE the terminal-state members' destructors (reverse member
    // order), so output_handle_ is still valid and raw mode is still on.
    if (mouse_enabled_ && output_handle_ != platform::invalid_handle) {
        static constexpr std::string_view kMouseOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_, kMouseOff);
        mouse_enabled_ = false;
    }
}

Runtime::Runtime(Runtime&& o) noexcept
    : alt_terminal_(std::move(o.alt_terminal_))
    , inline_terminal_(std::move(o.inline_terminal_))
    , output_handle_(std::exchange(o.output_handle_, platform::invalid_handle))
    , input_handle_(std::exchange(o.input_handle_, platform::invalid_handle))
    , resize_signal_(std::move(o.resize_signal_))
    , event_source_(std::move(o.event_source_))
    , writer_(std::move(o.writer_))
    , pool_(std::move(o.pool_))
    , canvas_(std::move(o.canvas_))
    , out_(std::move(o.out_))
    , layout_nodes_(std::move(o.layout_nodes_))
    , fs_coherence_(std::move(o.fs_coherence_))
    , in_coherence_(std::move(o.in_coherence_))
    , theme_(o.theme_)
    , size_(o.size_)
    , render_ctx_(o.render_ctx_)
    , resize_generation_(o.resize_generation_)
    , parser_(std::move(o.parser_))
    , running_(o.running_)
    , inline_top_row_(o.inline_top_row_)
    , inline_frame_rows_(o.inline_frame_rows_)
    , startup_events_(std::move(o.startup_events_))
{
    mouse_enabled_ = std::exchange(o.mouse_enabled_, false);
}

Runtime& Runtime::operator=(Runtime&& o) noexcept {
    if (this != &o) {
        alt_terminal_      = std::move(o.alt_terminal_);
        inline_terminal_   = std::move(o.inline_terminal_);
        output_handle_     = std::exchange(o.output_handle_, platform::invalid_handle);
        input_handle_      = std::exchange(o.input_handle_, platform::invalid_handle);
        resize_signal_     = std::move(o.resize_signal_);
        event_source_      = std::move(o.event_source_);
        writer_            = std::move(o.writer_);
        pool_              = std::move(o.pool_);
        canvas_            = std::move(o.canvas_);
        out_               = std::move(o.out_);
        layout_nodes_      = std::move(o.layout_nodes_);
        fs_coherence_      = std::move(o.fs_coherence_);
        in_coherence_      = std::move(o.in_coherence_);
        theme_             = o.theme_;
        size_              = o.size_;
        render_ctx_        = o.render_ctx_;
        resize_generation_ = o.resize_generation_;
        parser_            = std::move(o.parser_);
        running_           = o.running_;
        inline_top_row_    = o.inline_top_row_;
        inline_frame_rows_ = o.inline_frame_rows_;
        startup_events_    = std::move(o.startup_events_);
        mouse_enabled_     = std::exchange(o.mouse_enabled_, false);
    }
    return *this;
}

} // namespace maya::detail

// ============================================================================
// canvas_run — imperative canvas animation loop
// ============================================================================

namespace maya::detail {

Status canvas_run_impl(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint)
{
    using Clock = std::chrono::steady_clock;

    platform::set_ui_thread_priority();

    MAYA_TRY_DECL(auto cooked, Terminal<Cooked>::create());
    MAYA_TRY_DECL(auto raw, std::move(cooked).enable_raw_mode());
    auto input_h  = raw.input_handle();
    auto output_h = raw.output_handle();

    // Install resize signal handler.
    MAYA_TRY_DECL(auto resize_sig, platform::NativeResizeSignal::install());
    auto sig_handle = resize_sig.native_handle();

    std::optional<Terminal<AltScreen>> alt_term;
    std::optional<Terminal<Raw>>       raw_term;
    if (cfg.mode == Mode::Fullscreen) {
        MAYA_TRY_DECL(auto alt, std::move(raw).enter_alt_screen());
        alt_term = std::move(alt);
    } else {
        raw_term = std::move(raw);
    }

    if (!cfg.title.empty()) {
        auto seq = std::format("\x1b]0;{}\x07", cfg.title);
        (void)platform::io_write_all(output_h, seq);
    }

    // Mouse tracking sequences (platform-independent ANSI)
    static constexpr std::string_view kMouseOn  = "\x1b[?1003h\x1b[?1006h";
    static constexpr std::string_view kMouseOff = "\x1b[?1006l\x1b[?1003l";
    if (cfg.mouse) (void)platform::io_write_all(output_h, kMouseOn);

    Writer writer{output_h};

    Size sz;
    if (alt_term)      sz = alt_term->size();
    else if (raw_term) sz = raw_term->size();
    int W = std::max(1, sz.width.value);
    int H = std::max(1, sz.height.value);

    StylePool pool;
    on_resize(pool, W, H);

    Canvas front(W, H, &pool), back(W, H, &pool);
    front.mark_all_damaged();

    auto frame_ns   = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, cfg.fps));
    auto next_frame = Clock::now();

    // ── BSU recovery scope guard (linear-RAII) ─────────────────────────────
    // Frames are wrapped in DEC sync (\x1b[?2026h ... \x1b[?2026l). If a
    // partial write leaves the terminal in BSU and we never finish the
    // remainder (resize discards the rest, hard error returns), the
    // terminal stays in sync mode forever. A FrameScope is created when
    // a frame begins flushing; commit() suppresses recovery on success;
    // any other path runs ~FrameScope which emits sync_end + SGR reset.
    //
    // The TYPE-LEVEL invariant: PendingFrame OWNS a FrameScope, so dropping
    // the pending frame (for any reason) closes BSU. The compiler enforces
    // this — there is no API to construct PendingFrame without a scope, and
    // ~PendingFrame always runs ~FrameScope.
    static constexpr std::string_view kBsuRecovery = "\x1b[?2026l\x1b[0m";
    class FrameScope {
        platform::NativeHandle out_;
        bool                   may_be_open_ = false;
    public:
        FrameScope() noexcept : out_(platform::invalid_handle) {}
        explicit FrameScope(platform::NativeHandle h) noexcept
            : out_(h), may_be_open_(true) {}
        void commit() noexcept { may_be_open_ = false; }
        ~FrameScope() {
            if (may_be_open_ && out_ != platform::invalid_handle)
                (void)platform::io_write_all(out_, kBsuRecovery);
        }
        FrameScope(FrameScope&& o) noexcept
            : out_(std::exchange(o.out_, platform::invalid_handle))
            , may_be_open_(std::exchange(o.may_be_open_, false)) {}
        FrameScope& operator=(FrameScope&& o) noexcept {
            if (this != &o) {
                if (may_be_open_ && out_ != platform::invalid_handle)
                    (void)platform::io_write_all(out_, kBsuRecovery);
                out_         = std::exchange(o.out_, platform::invalid_handle);
                may_be_open_ = std::exchange(o.may_be_open_, false);
            }
            return *this;
        }
        FrameScope(const FrameScope&)            = delete;
        FrameScope& operator=(const FrameScope&) = delete;
    };

    struct PendingFrame {
        std::string data;
        std::size_t offset = 0;
        FrameScope  scope;          // closes BSU on drop unless commit()ed
    };
    std::optional<PendingFrame> pending_frame;
    std::string out;
    out.reserve(static_cast<std::size_t>(W * H * 12));

    InputParser parser;
    bool running     = true;
    bool needs_clear = true;

    // Create event source
    platform::NativeEventSource events(input_h, sig_handle);

    // RAII guards
    scope_exit mouse_guard([&] {
        if (cfg.mouse) (void)platform::io_write_all(output_h, kMouseOff);
    });

    auto handle_resize = [&](int nw, int nh) {
        W = nw; H = nh;
        on_resize(pool, W, H);
        front.resize(W, H);
        back.resize(W, H);
        front.mark_all_damaged();
        pending_frame.reset();
        needs_clear = true;
    };

    while (running) {
        auto now = Clock::now();
        int timeout_ms;
        if (now >= next_frame) {
            timeout_ms = 0;
        } else {
            auto wait  = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
            timeout_ms = static_cast<int>(wait.count()) + 1;
        }

        MAYA_TRY_DECL(auto ready, events.wait(
            std::chrono::milliseconds(timeout_ms),
            pending_frame.has_value()));

        if (ready.resize) {
            resize_sig.drain();
            // Coalesce rapid resizes (e.g. window drag) — drain all pending
            // resize events before rendering so we skip intermediate sizes.
            auto coalesce = [&] {
                Size new_sz;
                if (alt_term)      new_sz = alt_term->size();
                else if (raw_term) new_sz = raw_term->size();
                int nw = std::max(1, new_sz.width.value);
                int nh = std::max(1, new_sz.height.value);
                if (nw != W || nh != H) handle_resize(nw, nh);
            };
            coalesce();
            while (auto more = events.wait(std::chrono::milliseconds(0), false)) {
                if (!more->resize) break;
                resize_sig.drain();
                coalesce();
            }
        }

        if (pending_frame.has_value() && ready.writeable) {
            auto& pf   = *pending_frame;
            auto  sv   = std::string_view(pf.data).substr(pf.offset);
            auto  result = writer.write_some(sv);
            if (!result) return err(result.error());
            pf.offset += *result;
            if (pf.offset >= pf.data.size()) {
                pf.scope.commit();      // sync_end was at the tail; BSU closed
                std::swap(front, back);
                back.reset_damage();
                pending_frame.reset();
            }
        }

        if (ready.input) {
            Result<std::string> data_result = [&]() -> Result<std::string> {
                if (alt_term)      return alt_term->read_raw();
                else if (raw_term) return raw_term->read_raw();
                return ok(std::string{});
            }();
            if (!data_result) return std::unexpected{data_result.error()};
            auto& data = *data_result;
            if (!data.empty()) {
                for (auto& ev : parser.feed(data)) {
                    if (!on_event(ev)) { running = false; break; }
                }
            }
        }

        // Controlling terminal closed — drain any final bytes above, then
        // exit. Without this the next wait() returns immediately (kqueue
        // EV_EOF / poll POLLHUP stay latched), read() returns 0, and we
        // spin at 100% CPU until SIGKILL.
        if (ready.hangup) running = false;

        if (running && !pending_frame.has_value() && Clock::now() >= next_frame) {
            next_frame += frame_ns;
            if (Clock::now() > next_frame + frame_ns)
                next_frame = Clock::now() + frame_ns;

            if (cfg.auto_clear) {
                back.clear();
            } else {
                back.mark_all_damaged();
            }
            on_paint(back, W, H);

            out.clear();
            out += ansi::sync_start;
            if (needs_clear) {
                // Home cursor and overwrite every row — no \x1b[2J which
                // causes a visible flash. serialize() appends \x1b[K per
                // row to clear stale trailing content.
                out += "\x1b[H";
                serialize(back, pool, out);
                needs_clear = false;
            } else {
                diff(front, back, pool, out);
            }
            out += ansi::reset;
            out += ansi::sync_end;

            static const std::size_t kMinSize =
                ansi::sync_start.size() + ansi::reset.size() + ansi::sync_end.size();
            if (out.size() == kMinSize) {
                std::swap(front, back);
                back.reset_damage();
            } else {
                // FrameScope is armed BEFORE the write — if write_some
                // throws or the function returns early via err(...), the
                // guard's destructor still emits BSU recovery.
                FrameScope scope{output_h};
                auto result = writer.write_some(out);
                if (!result) return err(result.error());
                const std::size_t written = *result;
                if (written < out.size()) {
                    pending_frame = PendingFrame{out, written, std::move(scope)};
                } else {
                    scope.commit();
                    std::swap(front, back);
                    back.reset_damage();
                }
            }
        }
    }

    return ok();
}

} // namespace maya::detail
