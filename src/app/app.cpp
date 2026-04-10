#include "maya/app/app.hpp"

#include <algorithm>
#include <format>

#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"
#include "maya/platform/select.hpp"

#if MAYA_PLATFORM_MACOS
#include <pthread.h>
#endif

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

    // Fullscreen enters alt screen (consumes Raw terminal).
    // Inline stays in raw mode.
    std::optional<Terminal<AltScreen>> alt_term;
    std::optional<Terminal<Raw>>       raw_term;

    if (cfg.mode == Mode::Fullscreen) {
        MAYA_TRY_DECL(auto alt, std::move(raw).enter_alt_screen());
        alt_term = std::move(alt);
    } else {
        raw_term = std::move(raw);
    }

    // Create event source — multiplexes terminal input and resize signals.
    auto sig_handle = resize_sig.native_handle();
    platform::NativeEventSource event_source(input_h, sig_handle);

    Runtime rt;
    rt.alt_terminal_   = std::move(alt_term);
    rt.raw_terminal_   = std::move(raw_term);
    rt.output_handle_  = output_h;
    rt.input_handle_   = input_h;
    rt.resize_signal_  = std::move(resize_sig);
    rt.event_source_   = std::move(event_source);
    rt.writer_         = std::make_unique<Writer>(output_h);
    rt.theme_          = cfg.theme;

    // Set terminal title if provided.
    if (!cfg.title.empty()) {
        auto seq = std::format("\x1b]0;{}\x07", cfg.title);
        (void)platform::io_write_all(output_h, seq);
    }

    // Query initial terminal size.
    if (rt.alt_terminal_) {
        rt.size_ = rt.alt_terminal_->size();
    } else if (rt.raw_terminal_) {
        rt.size_ = rt.raw_terminal_->size();
    }

    rt.render_ctx_.width      = rt.size_.width.raw();
    rt.render_ctx_.height     = rt.size_.height.raw();
    rt.render_ctx_.generation = 0;

#if MAYA_PLATFORM_MACOS
    // Tell macOS scheduler this is a user-interactive thread.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    return ok(std::move(rt));
}

// ============================================================================
// Runtime::poll — wait for events on the multiplexed event source
// ============================================================================

auto Runtime::poll(std::chrono::milliseconds timeout) -> Result<PollResult> {
    MAYA_TRY_DECL(auto ready, event_source_->wait(timeout));
    return ok(PollResult{.resize = ready.resize, .input = ready.input});
}

// ============================================================================
// Runtime::handle_resize — update internal state on terminal resize
// ============================================================================

void Runtime::handle_resize() {
    if (resize_signal_) resize_signal_->drain();

    Size new_size;
    if (alt_terminal_) {
        new_size = alt_terminal_->size();
    } else if (raw_terminal_) {
        new_size = raw_terminal_->size();
    } else {
        return;
    }

    if (new_size != size_) {
        size_ = new_size;
        ++resize_generation_;
        render_ctx_.width      = size_.width.raw();
        render_ctx_.height     = size_.height.raw();
        render_ctx_.generation = resize_generation_;
        needs_clear_ = true;
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
    } else if (raw_terminal_) {
        MAYA_TRY_DECL(auto data, raw_terminal_->read_raw());
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
    const int w = is_inline()
        ? std::max(1, size_.width.raw() - 1)
        : size_.width.raw();
    if (w <= 0) return ok();

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = is_inline();
    RenderContextGuard ctx_guard(render_ctx_);

    if (is_inline()) {
        // ── Inline path: compose_inline_frame (row-diff renderer) ──────
        constexpr int kMaxCanvasHeight = 500;
        const int canvas_h = kMaxCanvasHeight;

        if (canvas_.width() != w || canvas_.height() != canvas_h)
            canvas_ = Canvas(w, canvas_h, &pool_);

        if (needs_clear_) {
            if (inline_state_.prev_rows > 0) {
                std::string erase;
                erase += "\x1b[2J\x1b[3J\x1b[H";
                (void)writer_->write_raw(erase);
            }
            inline_state_.reset();
            needs_clear_ = false;
        }

        if (inline_state_.prev_rows > 0) {
            canvas_.clear_rows(inline_state_.prev_rows + 4);
        } else {
            canvas_.clear();
        }
        render_tree(root, canvas_, pool_, theme_, layout_nodes_, /*auto_height=*/true);

        const int ch = content_height(canvas_);
        if (ch <= 0) {
            inline_state_.prev_rows = 0;
            return ok();
        }

        const int term_h = std::max(1, size_.height.raw());
        out_.clear();
        compose_inline_frame(canvas_, ch, term_h, pool_, inline_state_, out_);
        if (out_.empty()) return ok();

        auto write_result = writer_->write_raw(out_);
        if (!write_result) {
            inline_state_.reset();
            return write_result;
        }
    } else {
        // ── Fullscreen path: RenderPipeline type-state machine ─────────
        const int h = size_.height.raw();
        if (h <= 0) return ok();

        if (canvas_.width() != w || canvas_.height() != h) {
            canvas_ = Canvas(w, h, &pool_);
            front_  = Canvas(w, h, &pool_);
            front_.mark_all_damaged();
            needs_clear_ = true;
        }

        out_.clear();

        // RenderPipeline: Idle → Cleared → Painted → Opened
        auto opened = RenderPipeline<stage::Idle>::start(canvas_, pool_, theme_, out_)
            .clear()
            .paint(root, layout_nodes_)
            .open_frame();

        // Opened → (write_full | write_diff) → close_frame → Closed
        if (needs_clear_) {
            std::move(opened).write_full().close_frame();
            needs_clear_ = false;
        } else {
            std::move(opened).write_diff(front_).close_frame();
        }

        MAYA_TRY_VOID(writer_->write_raw(out_));
        std::swap(front_, canvas_);
        canvas_.reset_damage();
    }
    return ok();
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
    if (is_inline()) {
        auto cleanup = std::format("{}{}\r\n", ansi::show_cursor, ansi::reset);
        return writer_->write_raw(cleanup);
    }
    return ok();
}

// ============================================================================
// Runtime move constructor
// ============================================================================

Runtime::Runtime(Runtime&& o) noexcept
    : alt_terminal_(std::move(o.alt_terminal_))
    , raw_terminal_(std::move(o.raw_terminal_))
    , output_handle_(std::exchange(o.output_handle_, platform::invalid_handle))
    , input_handle_(std::exchange(o.input_handle_, platform::invalid_handle))
    , resize_signal_(std::move(o.resize_signal_))
    , event_source_(std::move(o.event_source_))
    , writer_(std::move(o.writer_))
    , pool_(std::move(o.pool_))
    , canvas_(std::move(o.canvas_))
    , front_(std::move(o.front_))
    , out_(std::move(o.out_))
    , inline_state_(std::move(o.inline_state_))
    , layout_nodes_(std::move(o.layout_nodes_))
    , theme_(o.theme_)
    , size_(o.size_)
    , render_ctx_(o.render_ctx_)
    , resize_generation_(o.resize_generation_)
    , parser_(std::move(o.parser_))
    , running_(o.running_)
    , needs_clear_(o.needs_clear_)
{}

Runtime& Runtime::operator=(Runtime&& o) noexcept {
    if (this != &o) {
        alt_terminal_      = std::move(o.alt_terminal_);
        raw_terminal_      = std::move(o.raw_terminal_);
        output_handle_     = std::exchange(o.output_handle_, platform::invalid_handle);
        input_handle_      = std::exchange(o.input_handle_, platform::invalid_handle);
        resize_signal_     = std::move(o.resize_signal_);
        event_source_      = std::move(o.event_source_);
        writer_            = std::move(o.writer_);
        pool_              = std::move(o.pool_);
        canvas_            = std::move(o.canvas_);
        front_             = std::move(o.front_);
        out_               = std::move(o.out_);
        inline_state_      = std::move(o.inline_state_);
        layout_nodes_      = std::move(o.layout_nodes_);
        theme_             = o.theme_;
        size_              = o.size_;
        render_ctx_        = o.render_ctx_;
        resize_generation_ = o.resize_generation_;
        parser_            = std::move(o.parser_);
        running_           = o.running_;
        needs_clear_       = o.needs_clear_;
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

#if MAYA_PLATFORM_MACOS
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

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

    struct PendingFrame { std::string data; std::size_t offset = 0; };
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
        pool.clear();
        on_resize(pool, W, H);
        front = Canvas(W, H, &pool);
        back  = Canvas(W, H, &pool);
        front.mark_all_damaged();
        pending_frame.reset();
        needs_clear = true;
        out.reserve(static_cast<std::size_t>(W * H * 12));
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
            Size new_sz;
            if (alt_term)      new_sz = alt_term->size();
            else if (raw_term) new_sz = raw_term->size();
            int nw = std::max(1, new_sz.width.value);
            int nh = std::max(1, new_sz.height.value);
            if (nw != W || nh != H) handle_resize(nw, nh);
        }

        if (pending_frame.has_value() && ready.writeable) {
            auto& pf   = *pending_frame;
            auto  sv   = std::string_view(pf.data).substr(pf.offset);
            auto  result = writer.write_some(sv);
            if (!result) return err(result.error());
            pf.offset += *result;
            if (pf.offset >= pf.data.size()) {
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
                out += "\x1b[2J\x1b[H";
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
                auto result = writer.write_some(out);
                if (!result) return err(result.error());
                const std::size_t written = *result;
                if (written < out.size()) {
                    pending_frame = PendingFrame{out, written};
                } else {
                    std::swap(front, back);
                    back.reset_damage();
                }
            }
        }
    }

    return ok();
}

} // namespace maya::detail
