#include "maya/app/app.hpp"

#include <algorithm>
#include <format>

#include "maya/core/focus.hpp"
#include "maya/core/simd.hpp"
#include "maya/core/overload.hpp"
#include "maya/core/scope_exit.hpp"
#include "maya/platform/select.hpp"

#if MAYA_PLATFORM_MACOS
#include <pthread.h>
#endif

namespace maya {

// ============================================================================
// App::Builder
// ============================================================================

auto App::Builder::mode(Mode m) -> Builder& {
    mode_ = m;
    return *this;
}

auto App::Builder::mouse(bool v) -> Builder& {
    mouse_ = v;
    return *this;
}

auto App::Builder::theme(Theme t) -> Builder& {
    theme_ = t;
    return *this;
}

auto App::Builder::title(std::string_view t) -> Builder& {
    title_ = std::string{t};
    return *this;
}

auto App::Builder::build() -> Result<App> {
    // Acquire the terminal and move it through the type-state chain.
    MAYA_TRY_DECL(auto cooked, Terminal<Cooked>::create());
    MAYA_TRY_DECL(auto raw, std::move(cooked).enable_raw_mode());

    auto input_h  = raw.input_handle();
    auto output_h = raw.output_handle();

    // Install resize signal handler.
    MAYA_TRY_DECL(auto resize_sig, platform::NativeResizeSignal::install());

    // Fullscreen mode enters the alt screen buffer (consumes the Raw terminal).
    // Inline mode stays in raw mode.
    std::optional<Terminal<AltScreen>> alt_term;
    std::optional<Terminal<Raw>>       raw_term;

    if (mode_ == Mode::Fullscreen) {
        MAYA_TRY_DECL(auto alt, std::move(raw).enter_alt_screen());
        alt_term = std::move(alt);
    } else {
        raw_term = std::move(raw);
    }

    // Construct the app.
    App app;
    app.alt_terminal_  = std::move(alt_term);
    app.raw_terminal_  = std::move(raw_term);
    app.output_handle_ = output_h;
    app.input_handle_  = input_h;
    app.resize_signal_  = std::move(resize_sig);
    app.writer_        = std::make_unique<Writer>(output_h);
    app.theme_          = theme_;
    app.mouse_enabled_  = mouse_;

    app.context_.set<Theme>(theme_);

    // Set terminal title if provided.
    if (!title_.empty()) {
        auto seq = std::format("\x1b]0;{}\x07", title_);
        (void)platform::io_write_all(output_h, seq);
    }

    // Query initial terminal size.
    if (app.alt_terminal_) {
        app.size_ = app.alt_terminal_->size();
    } else if (app.raw_terminal_) {
        app.size_ = app.raw_terminal_->size();
    }

    app.started_mode_ = mode_;
    app.render_ctx_.width      = app.size_.width.raw();
    app.render_ctx_.height     = app.size_.height.raw();
    app.render_ctx_.generation = 0;

    return ok(std::move(app));
}

// ============================================================================
// App::run
// ============================================================================

auto App::run(std::function<Element()> render_fn) -> Status {
    render_fn_ = std::move(render_fn);
    running_ = true;
    needs_render_ = true;
    return event_loop();
}

// ============================================================================
// Event loop — platform-abstracted
// ============================================================================

auto App::event_loop() -> Status {
    using Clock = std::chrono::steady_clock;
    auto next_frame = Clock::now();

#if MAYA_PLATFORM_MACOS
    // Tell macOS scheduler this is a user-interactive thread.
    // Gets priority on performance cores (M-series), reduces frame jitter.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    // Create the event source — multiplexes terminal input and resize signals.
    auto sig_handle = resize_signal_ ? resize_signal_->native_handle()
                                      : platform::invalid_handle;
    platform::NativeEventSource events(input_handle_, sig_handle);

    while (running_) {
        // Compute poll timeout
        int poll_timeout_ms = 100;
        if (fps_ > 0) {
            auto now = Clock::now();
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
            poll_timeout_ms = std::max(0, static_cast<int>(wait.count()));
        }

        MAYA_TRY_DECL(auto ready, events.wait(
            std::chrono::milliseconds(poll_timeout_ms)));

        if (ready.resize && resize_signal_) {
            resize_signal_->drain();
            handle_resize();
        }

        if (ready.input) {
            MAYA_TRY_VOID(read_and_dispatch());
        }

        for (auto& ev : parser_.flush_timeout()) {
            dispatch_event(ev);
        }

        if (fps_ > 0 && Clock::now() >= next_frame) {
            needs_render_ = true;
            next_frame += std::chrono::microseconds(1'000'000 / fps_);
            if (Clock::now() > next_frame)
                next_frame = Clock::now();
        }

        if (needs_render_) {
            MAYA_TRY_VOID(render_frame());
            needs_render_ = false;
        }
    }

    if (is_inline()) {
        auto cleanup = std::format("{}{}\r\n", ansi::show_cursor, ansi::reset);
        (void)writer_->write_raw(cleanup);
    }

    return ok();
}

// ============================================================================
// Input reading and dispatch
// ============================================================================

auto App::read_and_dispatch() -> Status {
    // Use the terminal's read method through the backend.
    if (alt_terminal_) {
        MAYA_TRY_DECL(auto data, alt_terminal_->read_raw());
        if (data.empty()) return ok();
        for (auto& event : parser_.feed(data)) {
            dispatch_event(event);
        }
    } else if (raw_terminal_) {
        MAYA_TRY_DECL(auto data, raw_terminal_->read_raw());
        if (data.empty()) return ok();
        for (auto& event : parser_.feed(data)) {
            dispatch_event(event);
        }
    }
    return ok();
}

void App::dispatch_event(Event& event) {
    std::visit(overload{
        [this](KeyEvent& ev) {
            if (current_focus_scope) {
                if (auto* sp = std::get_if<SpecialKey>(&ev.key)) {
                    if (*sp == SpecialKey::Tab) {
                        focus_next();
                        needs_render_ = true;
                        return;
                    }
                    if (*sp == SpecialKey::BackTab) {
                        focus_prev();
                        needs_render_ = true;
                        return;
                    }
                }
            }
            std::ranges::any_of(key_handlers_,
                [&](auto& h) { return h(ev); });
            needs_render_ = true;
        },
        [this](MouseEvent& ev) {
            std::ranges::any_of(mouse_handlers_,
                [&](auto& h) { return h(ev); });
            needs_render_ = true;
        },
        [this](ResizeEvent& ev) {
            size_ = {ev.width, ev.height};
            std::ranges::for_each(resize_handlers_,
                [&](auto& h) { h(size_); });
            needs_render_ = true;
        },
        [this](FocusEvent&) {
            needs_render_ = true;
        },
        [this](PasteEvent&) {
            needs_render_ = true;
        },
    }, event);
}

// ============================================================================
// Resize handling
// ============================================================================

void App::promote_to_alt_screen() {
    // Intentionally empty — inline mode stays inline to preserve
    // terminal scrollback. Alt screen has no scrollback.
}

void App::handle_resize() {
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
        context_.set<Size>(size_);
        ++resize_generation_;
        render_ctx_.width      = size_.width.raw();
        render_ctx_.height     = size_.height.raw();
        render_ctx_.generation = resize_generation_;

        needs_clear_ = true;

        for (auto& handler : resize_handlers_) {
            handler(size_);
        }
        needs_render_ = true;
    }
}

// ============================================================================
// Frame rendering
// ============================================================================

auto App::render_frame() -> Status {
    if (!render_fn_) return ok();

    const int w = is_inline()
        ? std::max(1, size_.width.raw() - 1)
        : size_.width.raw();
    if (w <= 0) return ok();

    render_ctx_.width       = w;
    render_ctx_.height      = size_.height.raw();
    render_ctx_.auto_height = is_inline();
    RenderContextGuard ctx_guard(render_ctx_);

    if (is_inline()) {
        constexpr int kMaxCanvasHeight = 500;
        const int canvas_h = kMaxCanvasHeight;

        if (canvas_.width() != w || canvas_.height() != canvas_h)
            canvas_ = Canvas(w, canvas_h, &pool_);

        // Invalidate state on clear or width change.
        if (needs_clear_ || prev_width_ != w) {
            if (prev_height_ > 0 && needs_clear_) {
                std::string erase;
                erase += "\x1b[2J\x1b[3J\x1b[H";
                (void)writer_->write_raw(erase);
            }
            prev_height_ = 0;
            prev_content_height_ = 0;
            row_hashes_.clear();
            needs_clear_ = false;
        }

        if (prev_content_height_ > 0) {
            canvas_.clear_rows(prev_content_height_ + 4);
        } else {
            canvas_.clear();
        }
        render_tree(render_fn_(), canvas_, pool_, theme_, layout_nodes_, /*auto_height=*/true);

        const int ch = content_height(canvas_);
        if (ch <= 0) {
            prev_content_height_ = 0;
            return ok();
        }

        const int W = canvas_.width();
        const uint64_t* cells = canvas_.cells();
        const int term_h = std::max(1, size_.height.raw());

        // ── Compute row hashes (reuse vector to avoid per-frame alloc) ──
        row_hashes_.swap(row_hashes_);  // keep old in row_hashes_ until swap
        // Save old hashes, compute new ones in-place.
        std::vector<uint64_t> old_hashes = std::move(row_hashes_);
        row_hashes_.resize(static_cast<size_t>(ch));
        for (int y = 0; y < ch; ++y)
            row_hashes_[static_cast<size_t>(y)] =
                simd::hash_row(cells + y * W, W);

        // ── Row-diff: never erase, only overwrite changed rows ──────────
        // Scrollback is never touched. User can scroll freely.

        const int prev_on_screen = std::min(prev_height_, term_h);
        const int common = std::min(ch, prev_height_);
        const int old_hash_count = static_cast<int>(old_hashes.size());

        // Only rows still on screen can be updated via cursor movement.
        // Rows in scrollback (index < prev_height_ - prev_on_screen)
        // are committed and can't be reached.
        const int updatable_start = prev_height_ - prev_on_screen;

        int first_changed = common; // assume no common rows changed
        for (int y = updatable_start; y < common && y < old_hash_count; ++y) {
            if (row_hashes_[static_cast<size_t>(y)] !=
                old_hashes[static_cast<size_t>(y)]) {
                first_changed = y;
                break;
            }
        }

        // If nothing changed and no new/removed rows, skip the write.
        if (first_changed == common && ch == prev_height_) {
            prev_content_height_ = ch;
            return ok();
        }

        out_.clear();
        out_ += ansi::sync_start;
        out_ += ansi::hide_cursor;

        if (prev_height_ == 0) {
            // First render: write all rows from cursor position.
            serialize(canvas_, pool_, out_, ch);
        } else {
            // Move cursor from bottom of previous output (row prev_height_-1)
            // up to first_changed. Clamp to on-screen distance.
            int up = prev_height_ - 1 - first_changed;
            up = std::clamp(up, 0, prev_on_screen - 1);
            if (up > 0)
                ansi::write_cursor_up(out_, up);
            out_ += '\r';

            // Overwrite from first_changed through end of new content.
            // serialize() writes \r\n between rows, \x1b[K after each row.
            serialize(canvas_, pool_, out_, ch, first_changed);

            // If content shrank, erase leftover rows below — but only
            // the ones still on screen (can't erase scrollback rows).
            if (ch < prev_height_) {
                int extra = std::min(prev_height_ - ch, prev_on_screen);
                for (int i = 0; i < extra; ++i)
                    out_ += "\r\n\x1b[2K";
                if (extra > 0)
                    ansi::write_cursor_up(out_, extra);
            }
        }

        out_ += ansi::reset;
        out_ += ansi::show_cursor;
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));
        prev_height_ = ch;
        prev_width_  = w;
        prev_content_height_ = ch;

    } else {
        const int h = size_.height.raw();
        if (h <= 0) return ok();

        if (canvas_.width() != w || canvas_.height() != h) {
            canvas_ = Canvas(w, h, &pool_);
            front_  = Canvas(w, h, &pool_);
            front_.mark_all_damaged();
            needs_clear_ = true;
        }

        canvas_.clear();
        render_tree(render_fn_(), canvas_, pool_, theme_);

        out_.clear();
        out_ += ansi::hide_cursor;
        out_ += ansi::sync_start;
        if (needs_clear_) {
            out_ += "\x1b[2J\x1b[H";
            serialize(canvas_, pool_, out_);
            needs_clear_ = false;
        } else {
            diff(front_, canvas_, pool_, out_);
        }
        out_ += ansi::reset;
        out_ += ansi::sync_end;

        MAYA_TRY_VOID(writer_->write_raw(out_));
        std::swap(front_, canvas_);
        canvas_.reset_damage();
    }
    return ok();
}

// ============================================================================
// canvas_run - Imperative canvas animation loop
// ============================================================================

namespace detail {

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

} // namespace detail
} // namespace maya
