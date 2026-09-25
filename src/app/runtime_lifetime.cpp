// src/app/runtime_lifetime.cpp — finalize, cleanup, destructor, moves.
#include "runtime_internal.hpp"

namespace maya::detail {

// ============================================================================
// Runtime::cleanup — final terminal cleanup
// ============================================================================

// ============================================================================
// Runtime::finalize_inline_frame — seal the inline chain, restore cursor
// ============================================================================

void Runtime::finalize_inline_frame() noexcept {
    if (!inline_terminal_) return;   // alt-screen path owes nothing here
    std::string buf;
    in_coherence_ = inline_frame::finalize_coherence(
        std::move(in_coherence_), buf);
    if (!buf.empty() && output_handle_ != platform::invalid_handle) {
        // Best effort through the writer first (keeps ordering with any
        // residue from the last frame), raw write as fallback.
        if (writer_) {
            (void)writer_->write_or_buffer(buf);
            for (int i = 0; i < 50 && writer_->has_residue(); ++i) {
                if (auto st = writer_->try_drain_residue(); !st) break;
                if (writer_->has_residue())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            (void)platform::io_write_all(output_handle_, buf);
        }
    }
}

auto Runtime::cleanup() -> Status {
    // ── Inline frame finalize ──────────────────────────────────
    // MUST run before the Terminal<InlineMode> destructor's teardown
    // bytes. The hardware-caret epilogue ends every frame with the
    // physical cursor AT THE CARET CELL — cursor_row_offset_ rows ABOVE
    // the frame's last wire row, inside the composer box. The terminal
    // destructor emits a bare \r\n to put the shell prompt on a fresh
    // line; issued from the caret row, that lands the prompt MID-BOX
    // and the shell's output then overwrites the remaining composer
    // rows in scrollback (the "agentty eats composer lines on close"
    // report). finalize_coherence emits exactly the bytes the state
    // knows it owes: CUD back to the resting row + \r, ?25h/?7h if
    // claimed, and the DECSCUSR `0 q` / OSC 112 cosmetic restores.
    // Idempotent — finalize on Sealed is a no-op, so the dtor calling
    // this again (exception path) emits nothing twice.
    finalize_inline_frame();
    // Disable mouse reporting if create() turned it on. The InlineMode /
    // AltScreen terminal destructors restore raw mode + screen state but
    // know nothing about mouse tracking, so without this the terminal is
    // left echoing SGR mouse reports (\x1b[<…M) as literal text into the
    // user's shell after the app exits. output_handle_ is still valid here
    // (cleanup runs before ~Runtime / the terminal destructors). Idempotent.
    if (mouse_enabled_) {
        static constexpr std::string_view kMouseOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_, kMouseOff);
        mouse_enabled_ = false;
    }
    // Focus reporting off — the shell doesn't expect CSI I/O on click.
    if (output_handle_ != platform::invalid_handle)
        (void)platform::io_write_all(output_handle_, ansi::disable_focus);
    // Pop the kitty keyboard protocol so the shell / next program sees the
    // key encoding it expects (leaving it pushed corrupts their input).
    if (kitty_kbd_enabled_ && output_handle_ != platform::invalid_handle) {
        (void)platform::io_write_all(output_handle_, ansi::kitty_keyboard_pop);
        kitty_kbd_enabled_ = false;
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
    // Inline-frame finalize first, same ordering rationale as cleanup():
    // the cursor must return to the resting row before the Terminal
    // dtor's \r\n. Idempotent (finalize on Sealed emits nothing).
    finalize_inline_frame();
    if (mouse_enabled_ && output_handle_ != platform::invalid_handle) {
        static constexpr std::string_view kMouseOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_, kMouseOff);
        mouse_enabled_ = false;
    }
    if (kitty_kbd_enabled_ && output_handle_ != platform::invalid_handle) {
        (void)platform::io_write_all(output_handle_, ansi::kitty_keyboard_pop);
        kitty_kbd_enabled_ = false;
    }
}

Runtime::Runtime(Runtime&& o) noexcept
    : alt_terminal_(std::move(o.alt_terminal_))
    , inline_terminal_(std::move(o.inline_terminal_))
    , output_handle_(std::exchange(o.output_handle_, platform::invalid_handle))
    , input_handle_(std::exchange(o.input_handle_, platform::invalid_handle))
    , resize_signal_(std::move(o.resize_signal_))
    , writer_(std::move(o.writer_))
    , pool_(std::move(o.pool_))
    , canvas_(std::move(o.canvas_))
    , out_(std::move(o.out_))
    , layout_nodes_(std::move(o.layout_nodes_))
    , grid_mode_(o.grid_mode_)
    , grid_need_full_(o.grid_need_full_)
    , retheme_repaint_(o.retheme_repaint_)
    , grid_prev_w_(o.grid_prev_w_)
    , grid_prev_rows_(o.grid_prev_rows_)
    , grid_committed_rows_(o.grid_committed_rows_)
    , grid_prev_content_h_(o.grid_prev_content_h_)
    , grid_prev_cells_(std::move(o.grid_prev_cells_))
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
    hover_motion_  = std::exchange(o.hover_motion_, false);
}

Runtime& Runtime::operator=(Runtime&& o) noexcept {
    if (this != &o) {
        alt_terminal_      = std::move(o.alt_terminal_);
        inline_terminal_   = std::move(o.inline_terminal_);
        output_handle_     = std::exchange(o.output_handle_, platform::invalid_handle);
        input_handle_      = std::exchange(o.input_handle_, platform::invalid_handle);
        resize_signal_     = std::move(o.resize_signal_);
        writer_            = std::move(o.writer_);
        pool_              = std::move(o.pool_);
        canvas_            = std::move(o.canvas_);
        out_               = std::move(o.out_);
        layout_nodes_      = std::move(o.layout_nodes_);
        grid_mode_         = o.grid_mode_;
        grid_need_full_    = o.grid_need_full_;
        retheme_repaint_   = o.retheme_repaint_;
        grid_prev_w_       = o.grid_prev_w_;
        grid_prev_rows_    = o.grid_prev_rows_;
        grid_committed_rows_ = o.grid_committed_rows_;
        grid_prev_content_h_ = o.grid_prev_content_h_;
        grid_prev_cells_   = std::move(o.grid_prev_cells_);
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
        hover_motion_      = std::exchange(o.hover_motion_, false);
    }
    return *this;
}

} // namespace maya::detail
