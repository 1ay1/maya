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
    // Cache the terminal-capability heuristic once. Cheap getenv() walk;
    // doing it lazily would just re-run it on every frame for no gain.
    rt.sync_output_     = ansi::env_supports_synchronized_output();

    // Set terminal title if provided.
    if (!cfg.title.empty()) {
        auto seq = std::format("\x1b]0;{}\x07", cfg.title);
        (void)platform::io_write_all(output_h, seq);
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
        size_ = new_size;
        ++resize_generation_;
        render_ctx_.width      = size_.width.raw();
        render_ctx_.height     = size_.height.raw();
        render_ctx_.generation = resize_generation_;
        // A resize invalidates both the prev-frame cell grid (fullscreen
        // diff) and the row-diff state (inline). Fullscreen collapses to
        // Divergent (next render does a full repaint). Inline transitions
        // to HardReset (next render emits \x1b[2J\x1b[3J\x1b[H and starts
        // fresh) when there's a Synced/Stale state to drop; Empty/Fresh
        // stay where they are because they haven't yet emitted anything.
        fs_coherence_ = coherent::Divergent{};
        in_coherence_ = std::visit(
            [](auto&& arm) -> inline_frame::InlineCoherence {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Synced>>) {
                    return std::move(arm).demote_to_hard_reset();
                } else if constexpr (std::is_same_v<T,
                        inline_frame::InlineFrame<inline_frame::Stale>>) {
                    return std::move(arm).escalate_to_hard_reset();
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

        if (canvas_.width() != w || canvas_.height() < kMinCanvasHeight) {
            canvas_.set_style_pool(&pool_);
            canvas_.resize(w, std::max(kMinCanvasHeight, canvas_.height()));
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
                        sync_output_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Fresh>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        sync_output_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Synced>>) {
                    auto wit = arm.verify();
                    if (!wit) {
                        return std::move(arm).demote_to_stale();
                    }
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        *std::move(wit), sync_output_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<Stale>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        sync_output_));
                }
                else if constexpr (std::is_same_v<T, InlineFrame<HardReset>>) {
                    return lift(std::move(arm).render(
                        canvas_, rows, term_h, pool_, *writer_,
                        sync_output_));
                }
                else {
                    static_assert(std::is_same_v<T, InlineFrame<Sealed>>);
                    return std::move(arm);   // sealed: no-op
                }
            }, std::move(in_coherence_));

        double cf_ms = since(t_cf0);
        if (prof) {
            std::fprintf(prof_out,
                "maya-frame: rt=%.2f cf=%.2f total=%.2f nodes=%zu rows=%d w=%d\n",
                rt_ms, cf_ms, since(t_frame_start),
                layout_nodes_.size(), ch, w);
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
                .open_frame(sync_output_);
            std::move(opened).write_diff(s.front).close_frame(sync_output_);
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
                .open_frame(sync_output_);
            // Home + serialize every row (no \x1b[2J — flashes inside DEC
            // sync). serialize() appends \x1b[K per row to wipe stale
            // trailing content.
            out_ += "\x1b[H";
            serialize(canvas_, pool_, out_);
            std::move(opened).close_frame(sync_output_);
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

// ============================================================================
// Runtime::cleanup — final terminal cleanup
// ============================================================================

auto Runtime::cleanup() -> Status {
    // No-op by design.  Both terminal states (Terminal<AltScreen>,
    // Terminal<Inline>) reverse their own opt-ins in their destructors,
    // so cleanup is structurally guaranteed by the type system — there
    // is no path where ~Runtime runs without the terminal being
    // restored.  This method is kept for ABI/API stability with
    // pre-type-state callers that still invoke (void)rt.cleanup().
    return ok();
}

// ============================================================================
// Runtime move constructor
// ============================================================================

Runtime::~Runtime() = default;

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
{}

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
