#pragma once
// maya::app - Application entry point and event loop
//
// Provides two entry points:
//
//   App  — element-tree / component model. Builder pattern, event handlers,
//           render callback that returns an Element. Uses serialize + erase-
//           and-redraw (Ink model). Good for UI applications.
//
//   canvas_run() — imperative canvas animation loop. Double-buffered diff,
//                  fixed-fps timer, POLLOUT retry. Good for games / animations.
//
// Both share the same terminal infrastructure (platform backends, signal
// handling, raw mode, alt screen). Resize events are delivered through the
// platform's NativeResizeSignal and NativeEventSource.

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/concepts.hpp"
#include "../core/expected.hpp"
#include "../core/render_context.hpp"
#include "../core/types.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../platform/select.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"
#include "context.hpp"

namespace maya {

// ============================================================================
// Mode — rendering mode selection
// ============================================================================

enum class Mode {
    Inline,      // Raw mode, no alt screen, scrollback preserved (Claude Code style)
    Fullscreen,  // Alt screen buffer, double-buffered cell diff
};

// ============================================================================
// App - The application entry point
// ============================================================================

class App {
public:
    // ========================================================================
    // Builder
    // ========================================================================

    class Builder {
        Mode        mode_       = Mode::Fullscreen;
        bool        mouse_      = false;
        Theme       theme_      = theme::dark;
        std::string title_;

    public:
        Builder() = default;

        auto mode(Mode m) -> Builder&;
        auto mouse(bool v) -> Builder&;
        auto theme(Theme t) -> Builder&;
        auto title(std::string_view t) -> Builder&;

        [[nodiscard]] auto build() -> Result<App>;
    };

    [[nodiscard]] static auto builder() -> Builder {
        return Builder{};
    }

    // ========================================================================
    // Running the app
    // ========================================================================

    template <Component C>
    auto run(C&& component) -> Status {
        return run([&component]() -> Element {
            return component.render();
        });
    }

    auto run(std::function<Element()> render_fn) -> Status;

    void quit() {
        running_ = false;
    }

    void set_fps(int fps) { fps_ = fps; }

    // ========================================================================
    // Event handlers
    // ========================================================================

    auto on_key(std::function<bool(const KeyEvent&)> handler) -> App& {
        key_handlers_.push_back(std::move(handler));
        return *this;
    }

    auto on_mouse(std::function<bool(const MouseEvent&)> handler) -> App& {
        mouse_handlers_.push_back(std::move(handler));
        return *this;
    }

    auto on_resize(std::function<void(Size)> handler) -> App& {
        resize_handlers_.push_back(std::move(handler));
        return *this;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    [[nodiscard]] bool is_inline() const noexcept { return raw_terminal_.has_value(); }
    [[nodiscard]] bool started_inline() const noexcept { return started_mode_ == Mode::Inline; }
    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] const ContextMap& context() const noexcept { return context_; }
    [[nodiscard]] ContextMap& context() noexcept { return context_; }

    // ========================================================================
    // Move-only
    // ========================================================================

    App(App&& o) noexcept
        : alt_terminal_(std::move(o.alt_terminal_))
        , raw_terminal_(std::move(o.raw_terminal_))
        , output_handle_(std::exchange(o.output_handle_, platform::invalid_handle))
        , input_handle_(std::exchange(o.input_handle_, platform::invalid_handle))
        , started_mode_(o.started_mode_)
        , resize_signal_(std::move(o.resize_signal_))
        , writer_(std::move(o.writer_))
        , pool_(std::move(o.pool_))
        , canvas_(std::move(o.canvas_))
        , front_(std::move(o.front_))
        , prev_height_(o.prev_height_)
        , prev_width_(o.prev_width_)
        , out_(std::move(o.out_))
        , prev_content_height_(o.prev_content_height_)
        , row_hashes_(std::move(o.row_hashes_))
        , layout_nodes_(std::move(o.layout_nodes_))
        , theme_(o.theme_)
        , mouse_enabled_(o.mouse_enabled_)
        , fps_(o.fps_)
        , context_(std::move(o.context_))
        , size_(o.size_)
        , render_ctx_(o.render_ctx_)
        , resize_generation_(o.resize_generation_)
        , parser_(std::move(o.parser_))
        , render_fn_(std::move(o.render_fn_))
        , running_(o.running_)
        , needs_render_(o.needs_render_)
        , needs_clear_(o.needs_clear_)
        , key_handlers_(std::move(o.key_handlers_))
        , mouse_handlers_(std::move(o.mouse_handlers_))
        , resize_handlers_(std::move(o.resize_handlers_))
    {}

    App& operator=(App&&) noexcept = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    ~App() = default;

private:
    App() = default;

    // -- Terminal ownership ---------------------------------------------------
    std::optional<Terminal<AltScreen>> alt_terminal_;
    std::optional<Terminal<Raw>>       raw_terminal_;
    platform::NativeHandle output_handle_ = platform::invalid_handle;
    platform::NativeHandle input_handle_  = platform::invalid_handle;
    Mode started_mode_ = Mode::Fullscreen;

    // -- Platform signal handling ---------------------------------------------
    std::optional<platform::NativeResizeSignal> resize_signal_;

    // -- Rendering pipeline ---------------------------------------------------
    std::unique_ptr<Writer> writer_;
    StylePool               pool_;
    Canvas                  canvas_;
    Canvas                  front_;
    int                     prev_height_ = 0;
    int                     prev_width_  = 0;
    std::string             out_;

    int                     prev_content_height_ = 0;
    std::vector<uint64_t>   row_hashes_;
    std::vector<layout::LayoutNode> layout_nodes_;

    // -- Configuration --------------------------------------------------------
    Theme      theme_         = theme::dark;
    bool       mouse_enabled_ = false;
    int        fps_           = 0;
    ContextMap    context_;
    Size          size_{};
    RenderContext render_ctx_;
    uint32_t      resize_generation_ = 0;

    // -- Event handling -------------------------------------------------------
    InputParser parser_;
    std::function<Element()> render_fn_;
    bool                     running_      = false;
    bool                     needs_render_ = true;
    bool                     needs_clear_  = false;

    std::vector<std::function<bool(const KeyEvent&)>>   key_handlers_;
    std::vector<std::function<bool(const MouseEvent&)>> mouse_handlers_;
    std::vector<std::function<void(Size)>>              resize_handlers_;

    // ========================================================================
    // Private methods (implemented in app.cpp)
    // ========================================================================

    auto event_loop() -> Status;
    auto read_and_dispatch() -> Status;
    void dispatch_event(Event& event);
    void handle_resize();
    void promote_to_alt_screen();
    auto render_frame() -> Status;
};

// ============================================================================
// canvas_run - Imperative canvas animation loop
// ============================================================================

struct CanvasConfig {
    int         fps        = 60;
    bool        mouse      = false;
    Mode        mode       = Mode::Fullscreen;
    bool        auto_clear = true;
    std::string title;
};

template <typename F>
concept CanvasResizeFn = std::invocable<F, StylePool&, int, int>;

template <typename F>
concept CanvasEventFn =
    std::invocable<F, const Event&> &&
    std::convertible_to<std::invoke_result_t<F, const Event&>, bool>;

template <typename F>
concept CanvasPaintFn = std::invocable<F, Canvas&, int, int>;

namespace detail {
[[nodiscard]] Status canvas_run_impl(
    CanvasConfig                                   cfg,
    std::function<void(StylePool&, int w, int h)>  on_resize,
    std::function<bool(const Event&)>              on_event,
    std::function<void(Canvas&, int w, int h)>     on_paint);
} // namespace detail

template <CanvasResizeFn ResizeFn, CanvasEventFn EventFn, CanvasPaintFn PaintFn>
[[nodiscard]] Status canvas_run(
    CanvasConfig cfg,
    ResizeFn&&   on_resize,
    EventFn&&    on_event,
    PaintFn&&    on_paint)
{
    return detail::canvas_run_impl(
        std::move(cfg),
        std::forward<ResizeFn>(on_resize),
        std::forward<EventFn>(on_event),
        std::forward<PaintFn>(on_paint));
}

} // namespace maya
