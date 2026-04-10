#pragma once
// maya::app — Type-theoretic application architecture
//
// The sole entry point for maya UI applications. Every application is a Program:
//
//   Model  — plain value type (your state). No signals, no shared_ptr, no mutation.
//   Msg    — closed sum type (variant). Every possible event is listed here.
//
//   init()              -> pair<Model, Cmd<Msg>>   — initial state + startup effects
//   update(Model, Msg)  -> pair<Model, Cmd<Msg>>   — pure state transition
//   view(const Model&)  -> Element                  — pure rendering
//   subscribe(const Model&) -> Sub<Msg>             — declarative event sources
//
// Output = Input + Effect. Effects are data (Cmd), never performed directly.
// The runtime interprets Cmd/Sub values and manages all I/O.
//
// Usage:
//
//   struct Counter {
//       struct Model { int count = 0; };
//       using Msg = std::variant<struct Inc, struct Dec, struct Quit>;
//       static auto init() -> std::pair<Model, Cmd<Msg>> { return {{}, Cmd<Msg>::none()}; }
//       static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> { ... }
//       static auto view(const Model& m) -> Element { ... }
//       static auto subscribe(const Model&) -> Sub<Msg> { ... }
//   };
//   static_assert(Program<Counter>);
//   int main() { maya::run<Counter>({.title = "counter"}); }
//
// For imperative canvas animations (games, demos), use canvas_run() at the
// bottom of this file — it is a low-level escape hatch, not the primary API.

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../core/cmd.hpp"
#include "../core/concepts.hpp"
#include "../core/expected.hpp"
#include "../core/overload.hpp"
#include "../core/render_context.hpp"
#include "../core/types.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../platform/select.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../render/pipeline.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"
#include "context.hpp"
#include "sub.hpp"

namespace maya {

// ============================================================================
// Mode — rendering mode selection
// ============================================================================

enum class Mode {
    Inline,      // Raw mode, no alt screen, scrollback preserved (Claude Code style)
    Fullscreen,  // Alt screen buffer, double-buffered cell diff
};

// ============================================================================
// RunConfig — application configuration
// ============================================================================
// C++20 aggregate. Add new fields with defaults — all existing call sites
// continue to compile unchanged.

struct RunConfig {
    std::string_view title      = "";               ///< Terminal window title (OSC 0)
    int              fps        = 0;                ///< Continuous rendering at N fps (0 = event-driven)
    bool             mouse      = false;            ///< Enable mouse event reporting
    Mode             mode       = Mode::Fullscreen; ///< Rendering mode
    Theme            theme      = theme::dark;      ///< Colour theme
};

// ============================================================================
// Key event predicates — pure functions for use inside subscribe() filters
// ============================================================================

[[nodiscard]] inline bool key_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.none();
}

[[nodiscard]] inline bool key_is(const KeyEvent& k, char32_t c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == c && k.mods.none();
}

[[nodiscard]] inline bool key_is(const KeyEvent& k, SpecialKey s) noexcept {
    auto* sk = std::get_if<SpecialKey>(&k.key);
    return sk && *sk == s && k.mods.none();
}

[[nodiscard]] inline bool ctrl_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.ctrl && !k.mods.alt;
}

[[nodiscard]] inline bool alt_is(const KeyEvent& k, char c) noexcept {
    auto* ck = std::get_if<CharKey>(&k.key);
    return ck && ck->codepoint == static_cast<char32_t>(c) && k.mods.alt && !k.mods.ctrl;
}

// ============================================================================
// key_map() — declarative keyboard subscription from a mapping table
// ============================================================================
// The most common subscription pattern: map keys to messages.
//
//   return key_map<Msg>({
//       {'q', Quit{}}, {'+', Increment{}},
//       {SpecialKey::Up, Increment{}},
//   });

using KeySpec = std::variant<char, SpecialKey>;

template <typename Msg>
[[nodiscard]] auto key_map(
    std::initializer_list<std::pair<KeySpec, Msg>> entries) -> Sub<Msg>
{
    auto vec = std::vector(entries.begin(), entries.end());
    return Sub<Msg>::on_key(
        [vec = std::move(vec)](const KeyEvent& k) -> std::optional<Msg> {
            for (const auto& [key, msg] : vec) {
                bool matched = std::visit(overload{
                    [&](char c)       { return key_is(k, c); },
                    [&](SpecialKey s) { return key_is(k, s); },
                }, key);
                if (matched) return msg;
            }
            return std::nullopt;
        });
}

// ============================================================================
// Program concept — the contract for a type-theoretic application
// ============================================================================
// A Program is a type that provides Model, Msg, and static pure functions.
//
// Required:
//   Model, Msg              — types
//   update(Model, Msg)      — returns pair<Model, Cmd<Msg>>
//   view(const Model&)      — returns Element
//
// Flexible:
//   init()                  — returns Model OR pair<Model, Cmd<Msg>>
//   subscribe(const Model&) — optional; omit for non-interactive apps
//
// Ergonomic shortcuts:
//   Cmd<Msg>{}              — same as Cmd<Msg>::none() (no effects)
//   return {new_model, {}}  — model + no effects (common case)

namespace detail {

template <typename P>
concept HasFullInit = requires {
    { P::init() } -> std::convertible_to<
        std::pair<typename P::Model, Cmd<typename P::Msg>>>;
};

template <typename P>
concept HasSimpleInit = requires {
    { P::init() } -> std::convertible_to<typename P::Model>;
};

template <typename P>
concept HasSubscribe = requires(const typename P::Model& m) {
    { P::subscribe(m) } -> std::convertible_to<Sub<typename P::Msg>>;
};

} // namespace detail

template <typename P>
concept Program = requires {
    typename P::Model;
    typename P::Msg;
} && (detail::HasFullInit<P> || detail::HasSimpleInit<P>)
  && requires(typename P::Model m, typename P::Msg msg) {
    { P::update(std::move(m), std::move(msg)) } -> std::convertible_to<
        std::pair<typename P::Model, Cmd<typename P::Msg>>>;
} && requires(const typename P::Model& m) {
    { P::view(m) } -> std::convertible_to<Element>;
};

// ============================================================================
// detail::Runtime — terminal resource owner (not public API)
// ============================================================================
// Owns the terminal, event source, writer, canvases, and render state.
// Exposes granular methods that run<P>() orchestrates into an event loop.

namespace detail {

class Runtime {
public:
    static auto create(RunConfig cfg) -> Result<Runtime>;

    void request_quit() noexcept { running_ = false; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] bool is_inline() const noexcept { return raw_terminal_.has_value(); }

    // Poll the event source for ready flags.
    struct PollResult { bool resize = false; bool input = false; };
    auto poll(std::chrono::milliseconds timeout) -> Result<PollResult>;

    // Handle a resize signal: drain, update size, mark needs_clear.
    void handle_resize();

    // Read terminal input, parse into events.
    auto read_events() -> Result<std::vector<Event>>;

    // Flush parser timeout events (e.g., bare Escape after delay).
    auto flush_timeouts() -> std::vector<Event>;

    // Render an element tree to the terminal.
    // Fullscreen: uses RenderPipeline (clear → paint → diff/serialize).
    // Inline: uses compose_inline_frame (row-diff, scrollback-preserving).
    auto render(const Element& root) -> Status;

    // Set terminal title via OSC 0.
    void set_title(std::string_view title);

    // Final cleanup (show cursor, reset, newline).
    auto cleanup() -> Status;

    // Move-only
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() = default;

private:
    Runtime() = default;

    // -- Terminal ownership ---------------------------------------------------
    std::optional<Terminal<AltScreen>> alt_terminal_;
    std::optional<Terminal<Raw>>       raw_terminal_;
    platform::NativeHandle output_handle_ = platform::invalid_handle;
    platform::NativeHandle input_handle_  = platform::invalid_handle;

    // -- Platform signal handling ---------------------------------------------
    std::optional<platform::NativeResizeSignal> resize_signal_;
    std::optional<platform::NativeEventSource>  event_source_;

    // -- Rendering pipeline ---------------------------------------------------
    std::unique_ptr<Writer> writer_;
    StylePool               pool_;
    Canvas                  canvas_;
    Canvas                  front_;
    std::string             out_;
    InlineFrameState        inline_state_;
    std::vector<layout::LayoutNode> layout_nodes_;

    // -- Configuration --------------------------------------------------------
    Theme         theme_              = theme::dark;
    Size          size_{};
    RenderContext render_ctx_;
    uint32_t      resize_generation_  = 0;

    // -- State ----------------------------------------------------------------
    InputParser  parser_;
    bool         running_      = true;
    bool         needs_clear_  = false;
};

} // namespace detail

// ============================================================================
// Cmd interpreter — executes effect descriptions
// ============================================================================
// The ONLY function that performs side effects from Cmd values.

namespace detail {

template <typename Msg>
struct CmdContext {
    Runtime&          rt;
    std::vector<Msg>& pending;

    struct TimerEntry {
        std::chrono::steady_clock::time_point fire_at;
        Msg                                    msg;
    };
    std::vector<TimerEntry>& timers;
};

template <typename Msg>
void execute_cmd(const Cmd<Msg>& cmd, CmdContext<Msg>& ctx) {
    std::visit(overload{
        [](const typename Cmd<Msg>::None&)  {},
        [&](const typename Cmd<Msg>::Quit&) { ctx.rt.request_quit(); },
        [&](const typename Cmd<Msg>::Batch& b) {
            for (auto& c : b.cmds) execute_cmd(c, ctx);
        },
        [&](const typename Cmd<Msg>::After& a) {
            ctx.timers.push_back({
                std::chrono::steady_clock::now() + a.delay,
                a.msg,
            });
        },
        [&](const typename Cmd<Msg>::SetTitle& s) {
            ctx.rt.set_title(s.title);
        },
        [&](const typename Cmd<Msg>::WriteClipboard& w) {
            (void)w; // TODO: implement OSC 52 clipboard write
        },
        [&](const typename Cmd<Msg>::Task& t) {
            t.run([&](Msg m) { ctx.pending.push_back(std::move(m)); });
        },
    }, cmd.inner);
}

// ============================================================================
// Sub interpreter — dispatches events through subscription filters
// ============================================================================

template <typename Msg>
void dispatch_through_sub(const Sub<Msg>& sub, const Event& ev,
                          std::vector<Msg>& out) {
    std::visit(overload{
        [](const typename Sub<Msg>::None&)  {},
        [&](const typename Sub<Msg>::Batch& b) {
            for (auto& s : b.subs) dispatch_through_sub(s, ev, out);
        },
        [&](const typename Sub<Msg>::OnKey& s) {
            if (auto* ke = std::get_if<KeyEvent>(&ev)) {
                if (auto msg = s.filter(*ke))
                    out.push_back(std::move(*msg));
            }
        },
        [&](const typename Sub<Msg>::OnMouse& s) {
            if (auto* me = std::get_if<MouseEvent>(&ev)) {
                if (auto msg = s.filter(*me))
                    out.push_back(std::move(*msg));
            }
        },
        [&](const typename Sub<Msg>::OnResize& s) {
            if (auto* re = std::get_if<ResizeEvent>(&ev)) {
                Size sz{re->width, re->height};
                out.push_back(s.on_resize(sz));
            }
        },
        [&](const typename Sub<Msg>::OnPaste& s) {
            if (auto* pe = std::get_if<PasteEvent>(&ev)) {
                out.push_back(s.on_paste(pe->content));
            }
        },
        [&](const typename Sub<Msg>::Every&) {
            // Timer subscriptions are handled by the timer loop, not event dispatch.
        },
    }, sub.inner);
}

// Collect all timer intervals from a Sub tree.
template <typename Msg>
void collect_timers(const Sub<Msg>& sub,
                    std::vector<std::pair<std::chrono::milliseconds, Msg>>& out) {
    std::visit(overload{
        [](const typename Sub<Msg>::None&)  {},
        [&](const typename Sub<Msg>::Batch& b) {
            for (auto& s : b.subs) collect_timers(s, out);
        },
        [&](const typename Sub<Msg>::Every& e) {
            out.push_back({e.interval, e.msg});
        },
        [](const auto&) {},
    }, sub.inner);
}

} // namespace detail

// ============================================================================
// run<P>() — the runtime for Program types
// ============================================================================
// The effectful shell: the ONLY place I/O happens.
//
// The loop:
//   1. Poll for events (terminal input, resize)
//   2. Dispatch events through Sub<Msg> → produce Msg values
//   3. For each Msg: update(model, msg) → (new model, Cmd)
//   4. Execute Cmd effects
//   5. Fire expired timers → more Msgs → repeat 3-4
//   6. Rebuild subscriptions from current model
//   7. view(model) → Element → render via RenderPipeline
//   8. Repeat

template <Program P>
void run(RunConfig cfg = {}) {
    using Model = typename P::Model;
    using Msg   = typename P::Msg;

    // Pure: initial state + optional startup effects
    Model model;
    Cmd<Msg> init_cmd{};  // default = none

    if constexpr (detail::HasFullInit<P>) {
        auto [m, c] = P::init();
        model    = std::move(m);
        init_cmd = std::move(c);
    } else {
        model = P::init();
    }

    // Effectful: create the runtime (terminal, event source, canvases)
    auto result = detail::Runtime::create(cfg);
    if (!result) {
        auto msg = std::format("maya: failed to initialize terminal: {}\n",
                               result.error().message);
        std::fputs(msg.c_str(), stderr);
        return;
    }
    auto rt = std::move(*result);

    // Interpreter bookkeeping (not part of Model — these are runtime state)
    std::vector<Msg> pending_msgs;
    std::vector<typename detail::CmdContext<Msg>::TimerEntry> timers;
    detail::CmdContext<Msg> ctx{rt, pending_msgs, timers};

    // Helper: drain all pending messages through update()
    auto drain_pending = [&] {
        while (!pending_msgs.empty()) {
            auto msgs = std::move(pending_msgs);
            pending_msgs.clear();
            for (auto& m : msgs) {
                auto [new_model, cmd] = P::update(std::move(model), std::move(m));
                model = std::move(new_model);
                detail::execute_cmd(cmd, ctx);
            }
        }
    };

    // Helper: get current subscriptions (or none if subscribe() isn't defined)
    auto get_sub = [&]() -> Sub<Msg> {
        if constexpr (detail::HasSubscribe<P>) {
            return P::subscribe(model);
        } else {
            return Sub<Msg>::none();
        }
    };

    // Execute startup effects
    if (!init_cmd.is_none()) {
        detail::execute_cmd(init_cmd, ctx);
        drain_pending();
    }

    // Current subscriptions (rebuilt after each update batch)
    Sub<Msg> current_sub = get_sub();

    // Fire initial resize so view() knows terminal dimensions
    {
        auto sz = rt.size();
        ResizeEvent re{sz.width, sz.height};
        Event ev{re};
        detail::dispatch_through_sub(current_sub, ev, pending_msgs);
        drain_pending();
        current_sub = get_sub();
    }

    bool needs_render = true;

    // ── Main event loop ──────────────────────────────────────────────────
    while (rt.is_running()) {
        // Compute poll timeout: min of 100ms, fps frame time, nearest timer
        auto poll_timeout = std::chrono::milliseconds(100);
        if (cfg.fps > 0) {
            poll_timeout = std::chrono::milliseconds(1000 / std::max(1, cfg.fps));
        }
        if (!timers.empty()) {
            auto now = std::chrono::steady_clock::now();
            for (auto& t : timers) {
                auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                    t.fire_at - now);
                poll_timeout = std::min(poll_timeout,
                    std::max(std::chrono::milliseconds(0), until));
            }
        }

        // Wait for events
        auto poll_result = rt.poll(poll_timeout);
        if (!poll_result) break;

        // Handle resize
        if (poll_result->resize) {
            rt.handle_resize();
            auto sz = rt.size();
            ResizeEvent re{sz.width, sz.height};
            Event ev{re};
            detail::dispatch_through_sub(current_sub, ev, pending_msgs);
        }

        // Read and dispatch terminal input
        if (poll_result->input) {
            auto events = rt.read_events();
            if (!events) break;
            for (auto& ev : *events) {
                detail::dispatch_through_sub(current_sub, ev, pending_msgs);
            }
        }

        // Flush parser timeouts (e.g., bare Escape)
        for (auto& ev : rt.flush_timeouts()) {
            detail::dispatch_through_sub(current_sub, ev, pending_msgs);
        }

        if (!pending_msgs.empty()) {
            drain_pending();
            needs_render = true;
        }

        // Fire expired timers
        {
            auto now = std::chrono::steady_clock::now();
            for (auto it = timers.begin(); it != timers.end(); ) {
                if (now >= it->fire_at) {
                    pending_msgs.push_back(std::move(it->msg));
                    it = timers.erase(it);
                } else {
                    ++it;
                }
            }
            if (!pending_msgs.empty()) {
                drain_pending();
                needs_render = true;
            }
        }

        // fps-driven continuous rendering
        if (cfg.fps > 0) needs_render = true;

        if (needs_render) {
            // Rebuild subscriptions from current model
            current_sub = get_sub();

            // Re-seed timer subscriptions
            auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<std::chrono::milliseconds, Msg>> timer_specs;
            detail::collect_timers(current_sub, timer_specs);
            for (auto& [interval, msg] : timer_specs) {
                bool already_pending = false;
                for (auto& t : timers) {
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        t.fire_at - now);
                    if (remaining <= interval) {
                        already_pending = true;
                        break;
                    }
                }
                if (!already_pending) {
                    timers.push_back({now + interval, std::move(msg)});
                }
            }

            // Pure: view(model) → Element → render to terminal
            auto status = rt.render(P::view(model));
            if (!status) break;
            needs_render = false;
        }
    }

    (void)rt.cleanup();
}

// ============================================================================
// canvas_run — imperative canvas animation loop (low-level escape hatch)
// ============================================================================
// For games, demos, and animations that need direct canvas access.
// NOT the primary API — use run<P>() for applications.

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
