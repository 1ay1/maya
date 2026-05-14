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

    // Schedule hint — see platform/thread.hpp. macOS: QoS user-interactive.
    // Linux/Win32: no-op.
    platform::set_ui_thread_priority();

    // Inline mode: start in InlineSynced with a fresh InlineFrameState
    // (prev_width = 0, prev_rows = 0). The first render's compose hits
    // the first-ever-render path with prev_width == 0, which does the
    // inline-mode growth from cursor's current position (no scrollback
    // wipe, host content stays visible above). Default-initialised
    // in_coherence_ would be Divergent, whose path emits
    // \x1b[2J\x1b[3J\x1b[H — appropriate for resize and write-fail
    // recovery, but wrong for startup. Pre-seeding InlineSynced
    // routes startup through the Synced case, preserving the host's
    // shell content above the live frame.
    if (rt.is_inline()) {
        // enable_inline_mode already emitted hide_cursor in the setup
        // sequence — pre-seed cursor_hidden=true so compose's first
        // frame doesn't redundantly re-emit it. DECAWM is left at the
        // host's default (on); compose will turn it off on its first
        // emit and the Terminal<InlineMode> destructor restores.
        InlineFrameState seed;
        seed.cursor_hidden = true;
        rt.in_coherence_ = coherent::InlineSynced{std::move(seed)};
    }

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
        // diff) and the row-diff state (inline) — collapse both coherence
        // variants to Divergent so the next render does a full repaint.
        // Dropping the FullscreenSynced/InlineSynced alternative releases
        // the front canvas / prev-frame InlineFrameState back to the heap.
        fs_coherence_ = coherent::Divergent{};
        in_coherence_ = coherent::Divergent{};
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

    const int w = is_inline()
        ? std::max(1, size_.width.raw() - 1)
        : size_.width.raw();
    if (w <= 0) return ok();

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = is_inline();
    RenderContextGuard ctx_guard(render_ctx_);

    if (is_inline()) {
        // No backpressure-based render skipping. A poll(POLLOUT)=full
        // result would skip composer keystrokes immediately after a
        // large turn emit on slow ttys (Pi-2 serial, ssh-over-high-RTT)
        // — replaying the first-keystroke-lag failure mode the
        // EMA-based coalescer hit. Keystroke responsiveness beats
        // streaming-burst coalescing: block-writing on a slow tty is
        // annoying but visible feedback per key is mandatory. Streaming
        // bursts coalesce naturally via compose_inline_frame's
        // no-change early return; the per-row diff path already keeps
        // emit bytes proportional to actual change.

        // ── Inline path: dispatch on coherence variant ──────────────────
        // std::visit selects the rendering function whose precondition
        // matches the current state.  The InlineFrameState (prev_cells +
        // prev_rows) lives only inside InlineSynced — compose_inline_frame
        // can ONLY be called from the Synced lambda where it has a state
        // to diff against.
        constexpr int kMinCanvasHeight = 500;

        if (canvas_.width() != w || canvas_.height() < kMinCanvasHeight) {
            canvas_.set_style_pool(&pool_);
            canvas_.resize(w, std::max(kMinCanvasHeight, canvas_.height()));
        }
        // Recover from any unmatched push_clip in the previous frame's
        // paint pass (e.g. a paint callback that threw past pop_clip).
        // Cheap — empties a vector — and guarantees the next paint
        // starts from an unbounded clip.
        canvas_.reset_clips();

        Status write_status = ok();
        in_coherence_ = std::visit(overload{
            // Divergent → Synced: erase the previously-rendered frame,
            // start a brand-new InlineFrameState, paint, write.
            [&](coherent::Divergent) -> coherent::InlineState {
                // Divergent is entered by resize (handle_resize sets
                // in_coherence_ = Divergent) or by a write-failure
                // recovery path. Both cases need a full reset of the
                // terminal viewport: the layout's width/height
                // assumptions just changed and any previous paint
                // is now in the wrong place. \x1b[2J clears the
                // viewport, \x1b[3J clears the saved-lines buffer
                // (per the user's call: "resize can wipe the
                // scrollback"), \x1b[H homes the cursor. Then
                // compose's first-ever-render path emits the fresh
                // frame at row 0 (case A in compose: prev_width == 0).
                //
                // STARTUP does NOT enter this path — the Runtime
                // constructor initialises in_coherence_ to
                // InlineSynced{} with a fresh InlineFrameState
                // (prev_width = 0). The first render hits the Synced
                // case below, compose's first-ever-render fires, and
                // because state.prev_width == 0 it does the
                // inline-mode growth (serialize from host's cursor
                // position). Host's terminal content stays visible
                // above; scrollback preserved.
                if (auto wr = writer_->write_raw("\x1b[2J\x1b[3J\x1b[H"); !wr) {
                    write_status = wr;
                    return coherent::Divergent{};
                }
                InlineFrameState fresh;
                canvas_.clear();
                render_tree(root, canvas_, pool_, theme_, layout_nodes_,
                            /*auto_height=*/true);
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
                if (ch <= 0) return coherent::InlineSynced{std::move(fresh)};

                const int term_h = std::max(1, size_.height.raw());
                out_.clear();
                compose_inline_frame(canvas_, ch, term_h, pool_, fresh, out_,
                                     sync_output_);
                if (out_.empty()) return coherent::InlineSynced{std::move(fresh)};

                if (auto wr = writer_->write_raw(out_); !wr) {
                    write_status = wr;
                    return coherent::Divergent{};   // back to unknown pixels
                }
                return coherent::InlineSynced{std::move(fresh)};
            },

            // Synced → Synced (success) or Synced → Divergent (write fail):
            // the InlineFrameState is owned here by `s.state`, so the
            // row-diff has access to its own canonical record.
            [&](coherent::InlineSynced& s) -> coherent::InlineState {
                // Full clear every frame. The bounded clear_rows(prev_rows + 4)
                // variant left cells past its horizon retaining content from
                // earlier frames; when a Turn's height shifted between frames
                // (streaming response, scroll, a new tool panel appearing) the
                // stale cells happened to match prev_cells at those rows so
                // compose_inline_frame's diff stayed silent and the terminal
                // kept displaying the previous frame's content — ghost
                // composers, duplicated markdown lines, broken vertical rails.
                // This mirrors the fix in detail::render_live (commit 489347b)
                // for the Runtime path, which run<Program> / run(event_fn,
                // render_fn) actually exercise.
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
                    s.state.prev_rows = 0;
                    return coherent::InlineSynced{std::move(s.state)};
                }

                const int term_h = std::max(1, size_.height.raw());
                out_.clear();
                auto t_cf0 = std::chrono::steady_clock::now();
                // Always emit every frame's diff (no bandwidth coalesce).
                //
                // The previous heuristic — hold tiny diffs (<=2 or <=4
                // rows) for up to kMaxConsecutiveHolds frames when
                // ns_per_byte_ema_ crossed slow-tty thresholds — saved
                // a handful of WriteFile calls during streaming bursts
                // but cost us correctness on the typing path. With
                // fps=0 a keystroke produces a single render, and a
                // 1-row composer-line change easily meets the
                // hold criterion: the first two keystrokes get
                // swallowed and only land when the third forces a
                // flush. On Windows conhost especially, WriteFile is
                // slow enough to push the EMA past 500 ns/B on the
                // first few frames, so the bug manifests immediately
                // and feels like "input is not instantaneous".
                //
                // Streaming reveal already paces renders at ~20 Hz
                // (50 ms tick); every terminal can handle that without
                // help. Pay the syscalls.
                compose_inline_frame(canvas_, ch, term_h, pool_, s.state, out_,
                                     sync_output_);
                double cf_ms = since(t_cf0);
                if (out_.empty()) {
                    if (prof) {
                        std::fprintf(prof_out,
                            "maya-frame: rt=%.2f cf=%.2f total=%.2f nodes=%zu rows=%d w=%d (skip-write)\n",
                            rt_ms, cf_ms, since(t_frame_start),
                            layout_nodes_.size(), ch, w);
                        std::fflush(prof_out);
                    }
                    return coherent::InlineSynced{std::move(s.state)};
                }

                auto t_w0 = std::chrono::steady_clock::now();
                auto wr = writer_->write_raw(out_);
                double w_ms = since(t_w0);
                if (!wr) {
                    write_status = wr;
                    return coherent::Divergent{};   // drop the stale state
                }
                if (prof) {
                    std::fprintf(prof_out,
                        "maya-frame: rt=%.2f cf=%.2f w=%.2f total=%.2f nodes=%zu out=%zu rows=%d w=%d\n",
                        rt_ms, cf_ms, w_ms, since(t_frame_start),
                        layout_nodes_.size(), out_.size(), ch, w);
                    std::fflush(prof_out);
                }
                return coherent::InlineSynced{std::move(s.state)};
            },
        }, in_coherence_);
        return write_status;
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
            if (auto wr = writer_->write_raw(out_); !wr) {
                write_status = wr;
                return coherent::Divergent{};       // drop the stale front
            }
            // Front ↔ back swap.  The just-written canvas content is now
            // the canonical front; the old front becomes the recyclable
            // back buffer for the next paint.
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
            if (auto wr = writer_->write_raw(out_); !wr) {
                write_status = wr;
                return coherent::Divergent{};
            }
            // canvas_ now mirrors what's on the terminal — promote it to
            // the new front.  Allocate a fresh back of matching size for
            // the next paint cycle.
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

void Runtime::set_title(std::string_view title) {
    auto seq = std::format("\x1b]0;{}\x07", title);
    (void)writer_->write_raw(seq);
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
