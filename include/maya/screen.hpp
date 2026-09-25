#pragma once
// maya/screen.hpp — the terminal as a device: maya without a runtime.
//
// A Screen owns the tty (through the type-state maya::Terminal<State>):
// raw mode, alt screen or inline, the input parser,
// the frame encoder and what it knows is on screen and in scrollback) and
// NOTHING that waits. Every method is one non-blocking step; there is no
// loop, no timer, no thread, no quit flag. A runtime (jaal, via maya-jaal,
// or a hand-written loop in a test) drives it:
//
//     auto term = maya::Screen::open({.mode = Mode::Inline});
//     watch(term->input_handle());                 // the runtime's reactor
//     ...on readable:   for (auto& ev : *term->read()) route(ev);
//     ...on a change:   Frame f = term->present(view(model));
//                       schedule(f.redraw_at);     // an animation wants more
//                       if (f.backpressured) watch_writable();
//     ...on writable:   term->flush();
//
// Why `Frame` is a return value: drawing has outputs a scheduler must act
// on (a widget asked to be drawn again; a scroll view learned its size and
// the frame used stale zeros; the tty refused bytes). They used to be
// thread-local globals every loop had to remember to read, and one of
// maya's three loops forgot two of them. A struct returned by the call that
// produces it can't be forgotten: using the frame means holding it.
//
// See docs/internals/runtime-free.md for the design.

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "app/app.hpp"

namespace maya {

/// What one draw needs from whoever schedules draws.
struct Frame {
    using clock = std::chrono::steady_clock;

    /// A widget asked to be drawn again (a spinner, a caret, a tween): call
    /// present() again at this time, even if nothing else changes.
    std::optional<clock::time_point> redraw_at;

    /// This frame measured a scroll view for the first time (or its extent
    /// moved), so it was drawn from stale sizes. Draw once more, now.
    bool redraw_now = false;

    /// The tty took only part of the frame. The rest is buffered: call
    /// flush() when the output is writable (or present() again).
    bool backpressured = false;

    /// Nothing visible changed, so nothing was drawn (the program's
    /// visual_hash matched and no widget asked for a frame).
    bool skipped = false;

    [[nodiscard]] bool wants_redraw() const noexcept { return redraw_now || redraw_at.has_value(); }
};

/// Device configuration: only what the TERMINAL needs. (Frame rate, key
/// maps and quitting are the runtime's business.)
struct TermConfig {
    std::string_view title             = "";
    Mode             mode              = Mode::Fullscreen;
    bool             mouse             = false;
    bool             hover_motion      = false;
    RenderBackend    backend           = RenderBackend::Ansi;
    Theme            theme             = theme::native;
    bool             enhanced_keyboard = true;
};

class Screen {
public:
    using clock = Frame::clock;

    /// Take the terminal: raw mode, alt screen or inline region, capability
    /// probes. The destructor gives it back, on every path.
    [[nodiscard]] static Result<Screen> open(const TermConfig& cfg) {
        RunConfig rc{};
        rc.title             = cfg.title;
        rc.mode              = cfg.mode;
        rc.mouse             = cfg.mouse;
        rc.hover_motion      = cfg.hover_motion;
        rc.backend           = cfg.backend;
        rc.theme             = cfg.theme;
        rc.enhanced_keyboard = cfg.enhanced_keyboard;
        auto rt = detail::Runtime::create(rc);
        if (!rt) return std::unexpected(rt.error());
        Screen t{std::move(*rt)};
        // Published only now: this is the address that lives (see
        // Runtime::publish_theme_slot on why create() can't).
        return t;
    }

    Screen(Screen&&) noexcept            = default;
    Screen& operator=(Screen&&) noexcept = default;
    Screen(const Screen&)                = delete;
    Screen& operator=(const Screen&)     = delete;
    ~Screen() { if (rt_) (void)rt_->cleanup(); }

    // ── input ────────────────────────────────────────────────────────────

    /// The handle a runtime watches for readability.
    [[nodiscard]] platform::NativeHandle input_handle() const noexcept { return rt_->input_handle(); }

    /// Everything the terminal has sent, parsed. Never blocks.
    [[nodiscard]] Result<std::vector<Event>> read() { return rt_->read_events(); }

    /// Call after SIGWINCH: re-reads the size and invalidates the frame.
    void on_resize() { rt_->handle_resize(); }
    [[nodiscard]] Size size() const noexcept { return rt_->size(); }

    // ── output ───────────────────────────────────────────────────────────

    /// Draw one frame: theme canvas, layout, paint, diff against what the
    /// terminal shows, write what the tty takes. Returns what the caller
    /// has to schedule.
    ///
    /// Takes the tree already built. Widgets ask for animation frames while
    /// they are BUILT, so a tree built before this call has already made
    /// its requests; to keep them, build through present(build) instead.
    Frame present(const Element& root) {
        Element framed = detail::apply_theme_canvas(root, rt_->theme(), rt_->size().width.value);
        (void)rt_->render(framed);
        return collect();
    }

    /// Build the tree and draw it, in the one order that works: clear the
    /// animation requests, THEN build (widgets request as they're built),
    /// THEN draw and collect them. A caller that builds first and clears
    /// after throws away every request, and animations freeze on their
    /// first frame (the jaal smoke run's `--animates` check catches that).
    template <std::invocable Build>
    Frame present(Build&& build) {
        detail::animation_requested_ = false;
        detail::next_frame_delay_ms_ = -1;
        return present(Element{std::forward<Build>(build)()});
    }

    /// Like present(), but builds the tree only if the caller's
    /// `visual_hash` moved, a widget asked to be redrawn, or `force` (the
    /// caller knows something the hash can't: a resize, a suspended child
    /// that scribbled on the screen). `build` is called at most once, AFTER
    /// the animation request is cleared, so the widgets it constructs can
    /// re-request.
    template <std::invocable Build>
    Frame present_if(std::uint64_t visual_hash, Build&& build, bool force = false) {
        if (!force && last_hash_ && *last_hash_ == visual_hash && !pending_redraw_
            && !rt_->has_deferred_frame() && !rt_->has_pending_writes()) {
            Frame f; f.skipped = true; return f;
        }
        last_hash_ = visual_hash;
        detail::animation_requested_ = false;
        detail::next_frame_delay_ms_ = -1;
        Element built  = std::forward<Build>(build)();
        Element framed = detail::apply_theme_canvas(std::move(built), rt_->theme(),
                                                    rt_->size().width.value);
        (void)rt_->render(framed);
        return collect();
    }

    /// Paint off-wire into the render cache (the one-shot warmup after a
    /// heavy model loads, so the first visible frame takes the blit path).
    void warm(const Element& root) { rt_->warmup_render(root); }

    /// Push bytes the tty refused earlier. False while it's still full.
    bool flush() {
        if (!rt_->has_pending_writes() && !rt_->has_deferred_frame()) return true;
        return !rt_->has_pending_writes();
    }
    [[nodiscard]] bool backpressured() const noexcept {
        return rt_->has_pending_writes() || rt_->has_deferred_frame();
    }

    // ── device effects: one call each ────────────────────────────────────

    void set_title(std::string_view t)             { rt_->set_title(t); }
    void write_clipboard(std::string_view s)       { rt_->write_clipboard(s); }
    void query_clipboard()                         { rt_->query_clipboard(); }
    void emit_host_sequence(std::string_view seq)  { rt_->emit_host_sequence(seq); }
    void commit_scrollback(ScrollbackDebt debt)    { rt_->commit_inline_prefix(debt.rows()); }
    void commit_overflow()                         { rt_->commit_inline_overflow(); invalidate(); }
    void reset_inline()                            { rt_->reset_inline(); invalidate(); }
    void force_redraw()                            { rt_->force_redraw(); invalidate(); }
    void set_mouse(bool on)                        { rt_->apply_mouse(on); }

    /// Hand the real tty to an interactive child (sudo, $EDITOR): the TUI
    /// is torn down to a cooked terminal, `run` executes, the TUI comes
    /// back and the next present() repaints from scratch. Returns what
    /// `run` returned.
    template <std::invocable F>
    auto suspend(F&& run) -> std::invoke_result_t<F&> {
        using R = std::invoke_result_t<F&>;
        invalidate();
        if constexpr (std::is_void_v<R>) {
            rt_->suspend([&] { run(); });
        } else {
            std::optional<R> out;
            rt_->suspend([&] { out.emplace(run()); });
            return std::move(*out);
        }
    }

    [[nodiscard]] bool is_inline() const noexcept { return rt_->is_inline(); }
    [[nodiscard]] const Theme& theme() const noexcept { return rt_->theme(); }

    /// The underlying implementation, for the runtimes being migrated off
    /// it. Not part of the device API.
    [[nodiscard]] detail::Runtime& impl() noexcept { return *rt_; }

private:
    explicit Screen(detail::Runtime rt) : rt_(std::make_unique<detail::Runtime>(std::move(rt))) {
        rt_->publish_theme_slot();
    }

    // Anything that repaints from scratch must defeat the hash gate.
    void invalidate() noexcept { last_hash_.reset(); }

    // Gather the draw's outputs from where the view layer leaves them. This
    // is the ONE place those globals are read, so no runtime has to.
    Frame collect() {
        Frame f;
        if (detail::animation_requested_) {
            const auto delay = detail::next_frame_delay_ms_ > 0
                ? std::chrono::milliseconds(detail::next_frame_delay_ms_)
                : detail::kAnimationFrameInterval;
            f.redraw_at = clock::now() + delay;
        }
        if (detail::scroll_writeback_dirty) {
            detail::scroll_writeback_dirty = false;
            f.redraw_now = true;
            last_hash_.reset();
        }
        f.backpressured  = rt_->has_pending_writes() || rt_->has_deferred_frame();
        pending_redraw_  = f.wants_redraw();
        return f;
    }

    // Heap-held: the Runtime publishes its own address (the theme slot), so
    // it must not move once published. The Screen handle can.
    std::unique_ptr<detail::Runtime> rt_;
    std::optional<std::uint64_t>     last_hash_;
    bool                             pending_redraw_ = false;
};

}  // namespace maya
