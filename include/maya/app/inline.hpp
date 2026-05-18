#pragma once
// maya::print / maya::live — Non-interactive rendering
//
// Two APIs for rendering maya elements without raw mode or input handling:
//
//   maya::print(element)
//       One-shot: render an element tree to stdout and return.
//       Great for styled CLI output — tables, reports, status cards.
//
//   maya::live({.fps = 30}, [&](float dt) {
//       return text("frame " + std::to_string(n++));
//   });
//       Live: re-renders inline at the given fps. Return the element tree
//       each frame. Call maya::quit() to stop the loop.
//
// Neither API uses the alternate screen buffer. Output stays in the
// terminal's scrollback like normal program output.

#include <chrono>
#include <concepts>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>

#include "../core/types.hpp"
#include "../element/element.hpp"
#include "../render/canvas.hpp"
#include "../render/inline_frame.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../platform/io.hpp"
#include "../terminal/ansi.hpp"
#include "../terminal/writer.hpp"

#include "quit.hpp"

namespace maya {

// ── Terminal width detection ─────────────────────────────────────────────────

namespace detail {

int detect_terminal_width() noexcept;
int detect_terminal_height() noexcept;

/// Persistent state for the inline renderer's erase-and-rewrite loop.
///
/// Move-only and field-private: the Witness Chain inside `frame_` is the
/// only legal entry point that may touch the cell grid / writer residue.
/// Outside code cannot reseat `frame_`, replay an old `writer_`, or stamp
/// a stale `canvas_width_` — every such hazard is now a compile error.
///
/// Lifecycle: `LiveState{}` → repeated `render_live(root, w, pool, std::move(state))`
/// → terminal `std::move(state).finalize(buf)`. Each step consumes the
/// previous value and returns the next; there is no in-place mutator path.
class [[nodiscard("LiveState owns the live inline frame — dropping it strands the witness chain and the writer's residue, corrupting the next render")]] LiveState {
public:
    // Pre-reserve so the first frame's tree build doesn't pay an
    // unbounded chain of vector reallocs. 1024 nodes covers typical
    // live-rendered trees; deeper trees still grow on demand.
    LiveState() { layout_nodes_.reserve(1024); }

    LiveState(const LiveState&)            = delete;
    LiveState& operator=(const LiveState&) = delete;
    LiveState(LiveState&&) noexcept            = default;
    LiveState& operator=(LiveState&&) noexcept = default;

    /// Consume the state and emit any finalization bytes (DECAWM /
    /// cursor restore) the witness chain still owes the wire.
    /// After this call the state is gone — accidental reuse is a
    /// use-after-move, caught by the variant's monostate.
    void finalize(std::string& buf) && {
        inline_frame::finalize_coherence(std::move(frame_), buf);
    }

private:
    Canvas                          canvas_;            // persistent canvas (avoids per-frame alloc)
    int                             canvas_width_ = 0;  // cached width — drives resize-demotion
    inline_frame::InlineCoherence   frame_ =
        inline_frame::InlineFrame<inline_frame::Empty>{};
    std::vector<layout::LayoutNode> layout_nodes_;

    // Writer is owned by LiveState rather than constructed per-call so
    // residue (bytes left over from a non-blocking partial write) is
    // preserved across render_live invocations. Dropping it between
    // frames would lose residue and let the next compose's prev_cells
    // reflect bytes the wire never received — the canonical inline-
    // corruption pattern the Witness Chain is designed to prevent.
    std::optional<Writer>           writer_;

    // render_live is the sole authorised mutator of these fields; the
    // live<> template loop chains LiveState values by move only.
    friend LiveState render_live(const Element& root, int width,
                                 StylePool& pool, LiveState state);
};

// Render element → serialize → write to stdout, preserving stable rows
// in scrollback. Consumes the state by value and returns the next one;
// callers must chain by move (`state = render_live(..., std::move(state));`).
[[nodiscard("render_live returns the next LiveState — dropping it loses the witness chain, the writer residue, and the cached canvas; the next frame will corrupt scrollback")]]
LiveState render_live(const Element& root, int width, StylePool& pool,
                      LiveState state);

} // namespace detail

// ── maya::print ──────────────────────────────────────────────────────────────

/// Print an element tree to stdout (one-shot, no event loop).
///
///   maya::print(
///       vstack()(
///           text("Hello", bold),
///           text("World", dim)
///       )
///   );
void print(const Element& root);

/// Print with explicit width.
void print(const Element& root, int width);

/// Render an element tree to a plain string (no ANSI escapes).
/// Useful for testing, CI output, and documentation generation.
[[nodiscard]] std::string render_to_string(const Element& root, int width = 80);

// ── LiveConfig ─────────────────────────────────────────────────────────────

struct LiveConfig {
    int   fps       = 30;      ///< Target frames per second
    int   max_width = 0;       ///< 0 = auto-detect terminal width
    bool  cursor    = false;   ///< Show cursor during rendering
};

// ── Render function concepts ─────────────────────────────────────────────────

template <typename F>
concept LiveRenderFnDt =
    std::invocable<F, float> &&
    std::convertible_to<std::invoke_result_t<F, float>, Element>;

template <typename F>
concept LiveRenderFnPlain =
    std::invocable<F> &&
    std::convertible_to<std::invoke_result_t<F>, Element>;

template <typename F>
concept AnyLiveRenderFn = LiveRenderFnDt<F> || LiveRenderFnPlain<F>;

// ── maya::live ─────────────────────────────────────────────────────────

/// Run an inline render loop. The render function is called each frame and
/// should return an Element. Call maya::quit() to stop.
///
///   int n = 0;
///   maya::live({.fps = 30}, [&](float dt) {
///       if (n++ > 100) maya::quit();
///       return text("Frame " + std::to_string(n));
///   });
///
/// Also accepts () -> Element (no delta time parameter):
///
///   maya::live({}, [&] {
///       return text("hello");
///   });
template <AnyLiveRenderFn RenderFn>
void live(LiveConfig cfg, RenderFn&& render_fn) {
    platform::ensure_utf8();
    detail::quit_requested = false;

    int width = cfg.max_width > 0
        ? cfg.max_width
        : detail::detect_terminal_width() - 1;
    if (width < 20) width = 20;

    auto frame_duration = std::chrono::microseconds(1000000 / std::max(1, cfg.fps));

    StylePool pool;
    std::string buf;
    detail::LiveState state;

    if (!cfg.cursor) std::fputs("\x1b[?25l", stdout); // hide cursor


    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();

    while (!detail::quit_requested) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        // Re-detect terminal width each frame to handle resize.
        if (cfg.max_width <= 0) {
            int new_w = std::max(20, detail::detect_terminal_width() - 1);
            if (new_w != width) width = new_w;
        }

        Element root = [&]() -> Element {
            if constexpr (LiveRenderFnDt<RenderFn>) {
                return render_fn(dt);
            } else {
                return render_fn();
            }
        }();

        if (detail::quit_requested) {
            // Final render before exit
            state = detail::render_live(root, width, pool, std::move(state));
            break;
        }

        state = detail::render_live(root, width, pool, std::move(state));

        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    // Restore DECAWM/cursor visibility owned by the inline frame state.
    // compose_inline_frame leaves DECAWM off across frames to save
    // bytes on slow ttys; finalize consumes the witness chain and
    // emits the restore. The state is gone after this call.
    buf.clear();
    std::move(state).finalize(buf);
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), stdout);

    if (!cfg.cursor) std::fputs("\x1b[?25h", stdout); // show cursor
    std::fputc('\n', stdout);
    std::fflush(stdout);

    detail::quit_requested = false;
}

} // namespace maya
