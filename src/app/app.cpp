#include "maya/app/app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>

#if MAYA_PLATFORM_POSIX || MAYA_PLATFORM_MACOS
    #include <unistd.h>   // isatty, getpid, fileno
#endif

#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"
#include "maya/platform/select.hpp"
#include "maya/platform/thread.hpp"
#include "maya/terminal/ansi.hpp"

namespace maya::detail {

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

    return ok(std::move(rt));
}

// ============================================================================
// Runtime::poll — wait for events on the multiplexed event source
// ============================================================================

auto Runtime::poll(std::chrono::milliseconds timeout) -> Result<PollResult> {
    MAYA_TRY_DECL(auto ready, event_source_->wait(timeout));
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

    if (alt_terminal_) {
        MAYA_TRY_DECL(auto data, alt_terminal_->read_raw());
        if (!data.empty()) {
            for (auto& event : parser_.feed(data))
                result.push_back(std::move(event));
        }
    } else if (inline_terminal_) {
        MAYA_TRY_DECL(auto data, inline_terminal_->read_raw());
        if (!data.empty()) {
            for (auto& event : parser_.feed(data))
                result.push_back(std::move(event));
        }
    }
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

        if (canvas_.width() != w
            || canvas_.height() < kMinCanvasHeight
            || oversized) {
            canvas_.set_style_pool(&pool_);
            const int target_h = oversized
                ? shrink_target
                : std::max(kMinCanvasHeight, canvas_.height());
            canvas_.resize(w, target_h);
        }
        // Recover from any unmatched push_clip in the previous frame's
        // paint pass (e.g. a paint callback that threw past pop_clip).
        canvas_.reset_clips();
        canvas_.clear();

        auto t_rt0 = std::chrono::steady_clock::now();
        render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                    /*auto_height=*/true);
        double rt_ms = since(t_rt0);

        int ch = content_height(canvas_);
        if (ch >= canvas_.height() && !layout_nodes_.empty()) {
            int needed = layout_nodes_[0].computed.size.height.raw();
            if (needed > canvas_.height()) {
                canvas_.resize(w, needed + 8);
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
                    // Shrink-while-overflowed guard. The incremental
                    // per-row diff is VIEWPORT-ONLY: it can rewrite the
                    // last `term_h` rows but never the rows that already
                    // scrolled into native scrollback. When the prior
                    // frame overflowed (prev_rows > term_h) and THIS
                    // frame is shorter, two outcomes are possible:
                    //
                    //   1. TURN-FINISH FREEZE. The live streaming tail
                    //      collapses into the frozen prefix; the shrink
                    //      happens at the BOTTOM. The rows already in
                    //      native scrollback (the frame's prefix) are
                    //      byte-IDENTICAL to before. The per-row diff's
                    //      own shrink branch handles this correctly and
                    //      append-only (re-emit bottom row + \x1b[J): no
                    //      committed row needs rewriting. Routing it
                    //      through commit+case-(B) instead RE-PAINTS the
                    //      viewport from content top, overlapping the
                    //      rows the prior frame committed at
                    //      prev_rows-term_h — stranding a duplicate copy
                    //      of the just-finished turn one screen up. This
                    //      is the "the turn doubles in scrollback when it
                    //      finishes" bug.
                    //
                    //   2. SCROLLBACK-CONTENT SHIFT. A card whose top is
                    //      already in scrollback shrinks at or above the
                    //      viewport top, so everything below shifts up
                    //      and the committed prefix rows no longer match
                    //      the new content. verify() can't catch this
                    //      (the shadow is internally consistent — it's
                    //      the WIRE that has stale committed rows). Here
                    //      a recovery IS needed: commit the off-viewport
                    //      rows and soft-repaint via case-(B). Preserves
                    //      host scrollback (no \x1b[3J wipe).
                    //
                    // Discriminate by comparing the new canvas's
                    // overflow-prefix rows against prev_cells: identical
                    // ⇒ case (1), take the diff path; different ⇒ case
                    // (2), recover. Row counts alone cannot tell them
                    // apart — both shrink while overflowed.
                    {
                        const int prev_rows = arm.rows();
                        const int new_rows  = rows.value();
                        if (prev_rows > term_h.value()
                            && new_rows < prev_rows) {
                            const int overflow = prev_rows - term_h.value();
                            const bool prefix_unchanged =
                                arm.scrollback_prefix_matches(canvas_, overflow);
                            if (!prefix_unchanged) {
                                // Genuine scrollback shift — commit the
                                // overflowed rows and soft-repaint.
                                verify_demoted = true;
                                auto marker = arm.scrollback_marker(overflow);
                                auto committed = std::move(arm).commit(marker);
                                return std::move(committed).demote_to_stale();
                            }
                            // prefix_unchanged: fall through to the diff
                            // path — append-only, scrollback-safe.
                        }
                    }
                    auto wit = arm.verify();
                    if (!wit) {
                        verify_demoted = true;
                        // The shadow is poisoned: prev_cells no longer
                        // matches the wire. How we recover depends on
                        // whether the live frame has overflowed the
                        // viewport into native scrollback.
                        //
                        //   • Fits within the viewport
                        //     (prev_rows <= term_h): no row has scrolled
                        //     off, so every diverged row is still on
                        //     screen and rewritable. Demote to Stale —
                        //     case (B) repaints the viewport in place,
                        //     no scrollback wipe, host content above
                        //     preserved. This is the cheap, common path.
                        //
                        //   • Overflowed the viewport
                        //     (prev_rows > term_h): part of the frame is
                        //     already committed to native scrollback,
                        //     which is immutable to us. case (B) is
                        //     VIEWPORT-ONLY — it cannot rewrite those
                        //     scrolled-off rows. A naive demote-to-Stale
                        //     would repaint the corrected viewport while
                        //     the stale (diverged) copy stays stranded
                        //     one screen up — the "turn repeats out of
                        //     nowhere in scrollback" symptom.
                        //
                        //     Recovery (NON-destructive): COMMIT the
                        //     already-overflowed rows to native
                        //     scrollback (advancing prev_rows down to
                        //     term_h via a scrollback_marker), THEN
                        //     demote to Stale so case (B) soft-repaints
                        //     the remaining viewport in place. The
                        //     committed rows are byte-identical to what's
                        //     physically on the wire (they overflowed
                        //     before the poison), so committing them
                        //     strands nothing; only the in-viewport rows
                        //     get rewritten. NO \x1b[3J wipe — host
                        //     scrollback is preserved. (An earlier
                        //     version hard-reset here, wiping pre-launch
                        //     scrollback; the commit+soft-repaint below
                        //     supersedes it and is strictly safer.)
                        //
                        // agent_session never hits the overflow branch:
                        // its live tail collapses to empty at MessageStop
                        // (frozen and live are mutually exclusive), so
                        // its frames stay viewport-sized. agentty's
                        // two-tier render keeps a tall live tail during
                        // streaming, which is exactly when a mid-scroll
                        // shadow poison can strand a duplicate.
                        const int prev_rows = arm.rows();
                        if (prev_rows > term_h.value()) {
                            // Overflowed + shadow poisoned: commit the
                            // off-viewport rows to native scrollback,
                            // then soft-repaint the viewport. Preserves
                            // host scrollback (no \x1b[3J wipe); the
                            // corrected viewport repaints in place via
                            // case (B).
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
    int ch = content_height(scratch);
    if (ch >= scratch.height() && !nodes.empty()) {
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
