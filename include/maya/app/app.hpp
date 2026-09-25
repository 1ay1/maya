#pragma once
// maya/app/app.hpp — the terminal device's internals (detail::Runtime), the
// key predicates, and the theme switch. Not a runtime: there is no loop
// here. maya::Screen (screen.hpp) is the public face of detail::Runtime,
// and <maya/app.hpp> runs a program on it through jaal.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../core/concepts.hpp"
#include "wire_coalesce.hpp"
#include "../core/expected.hpp"
#include "../core/function.hpp"
#include "../core/motion.hpp"
#include "../core/overload.hpp"
#include "../core/render_context.hpp"
#include "../core/scroll_state.hpp"
#include "../core/types.hpp"
#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../platform/select.hpp"
#include "../render/canvas.hpp"
#include "../render/diff.hpp"
#include "../render/inline_frame.hpp"
#include "../render/pipeline.hpp"
#include "../render/renderer.hpp"
#include "../render/serialize.hpp"
#include "../style/theme.hpp"
#include "../terminal/input.hpp"
#include "../terminal/terminal.hpp"
#include "../terminal/writer.hpp"

namespace maya {

/// Swap the running app's palette. Also valid before run<>() starts — the
/// theme is held until a Runtime adopts it, rather than dropped.
inline void app_set_theme(const Theme& t);   // defined below Runtime



// ============================================================================
// Mode — rendering mode selection
// ============================================================================

enum class Mode {
    Inline,      // Raw mode, no alt screen, scrollback preserved (Claude Code style)
    Fullscreen,  // Alt screen buffer, double-buffered cell diff
};

// How a rendered frame reaches the host.
//   Ansi — serialize the cell diff to ANSI escapes for a terminal (default).
//   Grid — emit the cell diff as a binary grid frame for a COOPERATING HOST
//          (an Emacs module, a GPU frontend) that paints cells directly,
//          skipping the ANSI encode+reparse round trip.  See render/grid_emit.
enum class RenderBackend {
    Ansi,
    Grid,
};

// ============================================================================
// Options — application configuration
// ============================================================================
// C++20 aggregate. Add new fields with defaults — all existing call sites
// continue to compile unchanged.

/// How to take the terminal: shared by Screen::open and maya::run.
struct Options {
    std::string_view title      = "";               ///< Terminal window title (OSC 0)
    int              fps        = 0;                ///< Continuous rendering at N fps (0 = event-driven)
    bool             mouse      = false;            ///< Enable mouse event reporting
    bool             hover_motion = false;          ///< Also report bare (no-button) motion (mode 1003)
                                                    ///< for hover highlights. Off by default: 1003 floods
                                                    ///< move events and some terminals handle it oddly.
    Mode             mode       = Mode::Fullscreen; ///< Rendering mode
    RenderBackend    backend    = RenderBackend::Ansi; ///< Frame transport (see RenderBackend)
    Theme            theme      = theme::native;      ///< Colour theme
    /// Negotiate the KITTY KEYBOARD PROTOCOL (progressive enhancement,
    /// flag 1 "disambiguate escape codes"). When the terminal supports it,
    /// modifier chords that legacy encoding cannot express — Ctrl+/, Ctrl+Tab,
    /// Shift+Enter, and the Esc-vs-escape-sequence ambiguity — arrive as
    /// unambiguous CSI-u events the parser already decodes. Terminals that
    /// don't understand it ignore the `\x1b[>1u` push (it's a private CSI),
    /// so this is safe to leave on; disable via env MAYA_NO_KITTY_KEYBOARD=1
    /// or by setting this false.
    bool             enhanced_keyboard = true;
};

// ============================================================================
// request_animation_frame — widget-side "I'm animating, please redraw soon"
// ============================================================================
// Widgets that want a smooth animation call request_animation_frame()
// from their build() each frame the animation should continue. This is
// the ONE animation engine: it does not maintain its own clock or poll
// schedule. It records, in a per-render registry, that *something on
// screen wants to step at ~frame cadence*. The run loop collects these
// requests right alongside Sub::Every timers and folds them into the
// single "when should I next wake and render" computation — no separate
// deadline global, no parallel pump, no second poll clamp.
//
// The distinction from Sub::Every is intent, not mechanism: Every
// delivers a Msg (drives update → model), a frame request only asks for
// a repaint (pure visual layer — cursor blink, scramble caret, sigil
// fade — that reads wall-clock in build() and mutates nothing). Both
// resolve to the same wake schedule.
//
// Single-threaded UI; the registry is a thread-local cleared at the
// start of each render (mirrors live_scroll_states). A widget that
// stops calling drops out of the next collection → loop returns to
// idle wait → zero bytes per idle frame. Idempotent within a frame.

namespace detail {
// ~60 fps cap — the cadence at which a frame request asks the loop to
// re-render. Single source of truth for animation frame timing.
inline constexpr auto kAnimationFrameInterval = std::chrono::milliseconds{16};

// Per-render frame-request flag. A widget's build() sets this; the run
// loop reads it after view()/render() to decide whether to schedule a
// follow-up frame, then clears it for the next render. Thread-local,
// single-threaded UI — same ownership model as live_scroll_states.
inline thread_local bool animation_requested_ = false;

// Companion to animation_requested_: the frame-delay policy for THIS render.
// Sentinel -1 = "unset" (no request yet this frame). 0 = at least one FAST
// requester (plain request_animation_frame / the motion framework's
// request_frame) wants the default ~16 ms / 60 fps cadence. A positive value
// = every requester so far opted into a minimum delay, and this is the
// SMALLEST such delay (the finest slow cadence any live widget needs).
//
// Merge rule (see the two setters below): a FAST request pins this to 0 and
// nothing can raise it again this frame — 60 fps for the welcome bob /
// spinner / streaming reveal must always win over a slow widget (the 265 ms
// caret blink) that happens to render in the same frame. Only when EVERY
// requester this frame asked for a delay does the loop sleep longer. Reset
// to -1 each render alongside animation_requested_.
inline thread_local std::int64_t next_frame_delay_ms_ = -1;

// ── Per-key frame fidelity ────────────────────────────────────────────
//
// A fast terminal delivers a whole key-repeat run in ONE read(), and the
// host folds every event but paints ONCE for the batch. For most
// input that is exactly right: the intermediate states are not interesting
// and painting them is wasted work.
//
// For NAVIGATION it is wrong, and it is what users report as "I hold Down,
// one press does nothing, then the next moves two rows". Nothing was
// dropped — the row was computed and overwritten before it reached the
// terminal. With two arrows per read the visible sequence is 2 → 4 → 6
// where the user pressed 1 2 3 4 5 6, so half the rows they steered
// through never existed on screen.
//
// The cursor position IS the feedback for an arrow key, so it has to be
// shown. This predicate marks the events whose intermediate frames are
// worth painting: plain arrows, Home/End, PageUp/PageDown, Tab. Everything
// else (typing, mouse motion, paste, resize) keeps batching, because there
// the end state is the only state anyone wants.
//
// Deliberately NOT "render every event": a paste arrives as hundreds of
// CharKeys and painting each one would turn a paste into a visible crawl.
[[nodiscard]] inline bool is_navigation_key(const Event& ev) noexcept {
    const auto* ke = std::get_if<KeyEvent>(&ev);
    if (!ke) return false;
    // A modified arrow is usually a different verb (word-jump, resize pane),
    // but it is still navigation and still wants its own frame. Ctrl/Alt
    // combos that are NOT arrows fall through to the default batching.
    if (const auto* sk = std::get_if<SpecialKey>(&ke->key)) {
        switch (*sk) {
            case SpecialKey::Up:
            case SpecialKey::Down:
            case SpecialKey::Left:
            case SpecialKey::Right:
            case SpecialKey::Home:
            case SpecialKey::End:
            case SpecialKey::PageUp:
            case SpecialKey::PageDown:
            case SpecialKey::Tab:
            case SpecialKey::BackTab:
                return true;
            default:
                return false;
        }
    }
    return false;
}

}  // namespace detail

inline void request_animation_frame() noexcept {
    detail::animation_requested_ = true;
    // A plain request wants the fast default cadence. Pin the policy to 0
    // (fast) so no slow request_animation_frame_after() this frame can raise
    // it — the welcome bob / spinner / streaming reveal always win over a
    // co-live slow widget (the caret blink).
    detail::next_frame_delay_ms_ = 0;
}

// Request the next frame no sooner than `delay_ms` from now (instead of the
// default ~16 ms). For SLOW, self-driving animations (the composer caret
// blink at its 265 ms half-period) so the run loop sleeps between visible
// steps rather than waking at 60 fps to re-check an effect that toggles a
// few times a second. Implies request_animation_frame().
//
// A delay only takes effect if EVERY requester this frame opted in: a single
// fast request (delay 0, pinned by request_animation_frame) dominates and is
// never raised. Among competing delays the SMALLEST wins (finest slow
// cadence any live widget needs).
inline void request_animation_frame_after(std::int64_t delay_ms) noexcept {
    detail::animation_requested_ = true;
    if (delay_ms < 0) delay_ms = 0;
    auto& cur = detail::next_frame_delay_ms_;
    if (cur == 0) return;                 // a fast requester already won
    if (cur < 0 || delay_ms < cur) cur = delay_ms;   // unset, or a finer delay
}

// Is a widget already driving the next frame itself?
//
// True when something called request_animation_frame(_after) during the last
// build(). Those frames are guaranteed to render — the run loop's RAF
// override bypasses the visual-hash gate for exactly this case — and the
// loop is already scheduled to wake on the requesting widget's own cadence.
//
// This exists so a host's visual_hash can DEFER instead of guessing. The
// hash's job is to describe host-paced state: model fields, and time buckets
// for animations the host itself drives with a timer. For a widget-paced
// (RAF) visual it must add NO time term — a host bucket is a second,
// independent clock for one visual, and unless it exactly matches the
// widget's interval the two beat, so renders land mid-step and smooth motion
// turns into stutter.
//
// The rule a host can now express directly rather than remember:
//
//     if (!maya::animation_pending()) {
//         // only bucket time for animations WE pace
//         mix(now_ms / our_timer_period);
//     }
//
// agentty hit the failure this prevents: it bucketed the RAF-driven welcome
// screen at its own 80 ms tick while the widget asked for 110 ms, producing
// 42 hash values for 30 requested frames and a visibly flickering idle
// screen.
[[nodiscard]] inline bool animation_pending() noexcept {
    return detail::animation_requested_;
}

// Wire the decoupled motion-framework frame-request hook to the host. anim::Motion / Timeline / pulse (core/motion.hpp) wake the loop
// WITHOUT depending on this 90 KB header by routing through
// anim::detail::raf_hook, installed here at static-init so any TU that links
// the app gets self-driving animations for free.
namespace detail {
inline void raf_thunk_() noexcept { ::maya::request_animation_frame(); }
inline void raf_after_thunk_(std::int64_t delay_ms) noexcept {
    ::maya::request_animation_frame_after(delay_ms);
}
inline const ::maya::anim::detail::RafInstaller raf_installer_{
    &raf_thunk_, &raf_after_thunk_};
} // namespace detail

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


namespace detail {
// Optional Program::visual_hash detector. When a Program type defines
// `static std::uint64_t visual_hash(const Model&)`, the host
// hashes the model just before calling view() and skips the view()
// + render() pair when the hash is unchanged since the last render.
// Cuts the wasted work for Tick-driven wakeups whose deltas don't
// affect anything visible (smoothing pacer drained 0 bytes, spinner
// frame unchanged because the bucket didn't roll over, status
// toast already cleared).
template <typename P, typename = void>
struct HasVisualHash : std::false_type {};
template <typename P>
struct HasVisualHash<P, std::void_t<decltype(
    P::visual_hash(std::declval<const typename P::Model&>()))>>
    : std::true_type {};

// Optional Program::needs_warmup detector. When a Program type defines
// `static bool needs_warmup(const Model&)` AND it returns true for the
// current model, the host performs an off-wire warmup_render of
// the same view BEFORE the user-visible render. The warmup populates
// maya's hash-keyed component cache; the user-visible render then
// takes the cell-blit fast path. Burns one extra render() worth of
// CPU off-frame to convert a tens-to-hundreds-of-ms cold paint into a
// sub-millisecond warm paint — the right trade after a model swap
// that loads a large frozen scrollback (agentty thread resume).
//
// The Program is responsible for clearing the flag on the next reducer
// step so warmup fires exactly once per swap; leaving it stuck on
// would double every frame's render cost.
template <typename P, typename = void>
struct HasNeedsWarmup : std::false_type {};
template <typename P>
struct HasNeedsWarmup<P, std::void_t<decltype(
    P::needs_warmup(std::declval<const typename P::Model&>()))>>
    : std::true_type {};
} // namespace detail


// ============================================================================
// detail::Runtime — terminal resource owner (not public API)
// ============================================================================
// Owns the terminal, event source, writer, canvases, and render state.
// Exposes granular methods; maya::Screen is its public face and the jaal
// host (<maya/app.hpp>) drives it one non-blocking step at a time.

namespace detail {

// ============================================================================
// Render-coherence type-state (parameterized double-buffer)
// ============================================================================
// The terminal/canvas relationship has two genuine states, encoded as the
// alternatives of a std::variant.  This is the parameterized-canvas form:
// the *only* path that carries the canonical front buffer is the Synced
// alternative — Divergent literally has no Canvas member.  An incremental
// diff therefore cannot be issued from a Divergent state because the front
// buffer required by the diff routine is physically inaccessible.
//
//   FullscreenSynced { Canvas front; }
//       Terminal pixels are known to match `front`.  The next render can
//       diff (canvas_ vs front) and emit only the changed cells.  Producer:
//       a successful end-to-end frame write.
//
//   InlineSynced { InlineFrameState state; }
//       Inline analogue: `state.prev_cells/prev_rows` accurately model
//       the current scrollback tail.  compose_inline_frame() can do its
//       row-diff against this state.
//
//   Divergent {}
//       Terminal pixels are unknown — write failed mid-frame, resize
//       happened, or this is the first render.  The only legal next move
//       is a full serialize / clear-and-paint, which (on success) returns
//       to Synced.
//
// std::visit on either variant gives compile-time exhaustiveness: adding
// a new state forces every dispatcher to handle it or fail to build.
//
// INLINE COHERENCE NOTE
// ————————————————————————————————————————————
// The inline path no longer uses `coherent::InlineState`. It now uses
// `maya::inline_frame::InlineCoherence` (the Witness Chain), which has
// six type-tagged states (Empty / Fresh / Synced / Stale / HardReset /
// Sealed) instead of the previous two (InlineSynced / Divergent). The
// chain enforces every render precondition at compile time — see
// docs/internals/witness-chain.md. The legacy InlineSynced/Divergent
// types remain for FULLSCREEN coherence; inline mode uses InlineCoherence.
namespace coherent {

struct FullscreenSynced {
    Canvas front;   // canonical "what the terminal currently displays"
};

struct Divergent {};

using FullscreenState = std::variant<FullscreenSynced, Divergent>;

} // namespace coherent

class Runtime {
public:
    static auto create(Options cfg) -> Result<Runtime>;

    void request_quit() noexcept { running_ = false; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

    /// Swap the palette mid-run.
    ///
    /// A theme picked from a settings screen has to take effect on the NEXT
    /// frame, not the next launch: a user judges a theme by looking at it,
    /// and a restart between choosing and seeing makes that impossible.
    /// The theme is only read during render, so replacing it between frames
    /// is safe — and every frame is a pure function of model + theme, so
    /// nothing caches a colour across the swap.
    void set_theme(const Theme& t) noexcept { theme_ = t; on_theme_changed(theme_); }

    /// Publish this runtime's theme slot to app_set_theme().
    ///
    /// Two sources can name the starting theme, and exactly one wins:
    ///
    ///   * app_set_theme() called before any Runtime exists (it parks the
    ///     theme in its own storage). That is the host saying "use THIS",
    ///     after reading its own config, so it must not be reverted.
    ///   * Options::theme, stored in theme_ by create().
    ///
    /// Adoption used to be `theme_ = theme::live()` unconditionally, which
    /// honoured the first and silently threw the second away: live() is
    /// native when nobody set anything, so run<App>({.theme = dracula})
    /// started in native.
    /// Whether anyone set a theme pre-runtime is exactly what live_epoch()
    /// counts, so that decides it, rather than comparing against native (a
    /// host may deliberately set native over a Options default).
    void publish_theme_slot() noexcept {
        if (theme::live_epoch() != 0) theme_ = theme::live();
        live_theme() = &theme_;
        on_theme_changed(theme_);
    }

    /// The theme the CURRENT run paints with, for hosts whose update path is
    /// pure and so cannot hold a reference to the runtime.
    ///
    /// An Elm-shaped host keeps update() free of the runtime on purpose, but
    /// a theme swap still has to reach the renderer. Rather than thread a
    /// mutable runtime through every reducer — which would hand every one of
    /// them the ability to repaint — the swap goes through this one named
    /// seam, so the places that can change the palette stay greppable.
    static Theme*& live_theme() noexcept {
        static Theme* t = nullptr;
        return t;
    }

    /// Subscribers notified whenever the live theme is replaced.
    ///
    /// Most of maya's colour is resolved per-frame straight from the Theme,
    /// so it needs no notification. A few subsystems instead keep a
    /// PROJECTED palette — markdown's ~35 `colors::` globals are the main
    /// one — because their render path is hot and reads a flat struct
    /// rather than walking a Theme. Those have to be re-derived when the
    /// theme changes, or a scheme repaints the chrome while the prose,
    /// code spans and tables stay on the old palette. That half-themed
    /// result is exactly what makes a theme picker feel broken.
    ///
    /// A hook rather than a direct call because those subsystems live
    /// ABOVE this header (markdown.hpp is opt-in; app.hpp must not drag it
    /// in). Each registers itself once; the runtime does not know who they
    /// are, only that they must be told.
    static std::vector<void (*)(const Theme&)>& theme_subscribers() {
        static std::vector<void (*)(const Theme&)> subs;
        return subs;
    }

    static void on_theme_changed(const Theme& t) {
        // Re-seat the style layer's view of the theme. Slot-kind colours
        // (every themed widget Config default) resolve against this at
        // SGR-emit time, which is below the app layer and cannot reach a
        // Runtime — so it gets its own pointer to the same object.
        //
        // This also bumps the theme EPOCH, which is what refreshes every
        // theme::projected<P> palette. Those used to need a subscriber each
        // (see below); now deriving is their read path, so a projection that
        // "forgets to register" is not expressible.
        theme::set_live(t);
        // Drop every cross-frame cached component.
        //
        // The renderer blits a cached subtree's PIXELS — colours already
        // resolved — keyed on a content hash. A theme swap changes none of
        // that content, so without this every cached turn, panel and row
        // keeps painting the old palette until its content happens to
        // change. That is the difference between "the new frame is themed"
        // and "the screen is themed", and it is why a theme change looked
        // like it only touched whatever was being redrawn anyway.
        //
        // Cost is one repaint of visible content on a swap, which is
        // exactly what the user asked for by picking a theme.
        render_detail::clear_component_cache();
        for (auto* fn : theme_subscribers()) fn(t);
    }
    [[nodiscard]] bool is_inline() const noexcept { return inline_terminal_.has_value(); }

    // Row offset for translating absolute SGR mouse coordinates into
    // frame-relative coordinates in inline mode. In inline mode the frame is
    // drawn partway down the terminal (at inline_top_row_, learned via a
    // cursor-position query at create()), but the terminal reports mouse
    // events in absolute rows. Callers subtract this from a mouse event's row
    // so hit-testing (scroll states, on_click, mouse_pos) lines up with the
    // rendered UI, exactly as it already does in fullscreen mode. Returns 0
    // when fullscreen, or when the anchor is unknown (query unanswered), in
    // which case behavior is unchanged.
    [[nodiscard]] int inline_mouse_dy() const noexcept {
        return (is_inline() && inline_top_row_ > 0) ? inline_top_row_ - 1 : 0;
    }
    // Content height (rows) of the last inline frame; used to drop mouse
    // events that fall outside the frame so the app doesn't react to clicks
    // in the surrounding scrollback. 0 = unknown (don't suppress).
    [[nodiscard]] int inline_frame_rows() const noexcept { return inline_frame_rows_; }

    // Enable/disable terminal mouse reporting at runtime. Drives the public
    // maya::set_mouse(bool); keeps mouse_enabled_ in sync so cleanup() emits
    // the matching disable on exit.
    void apply_mouse(bool on) noexcept {
        if (output_handle_ == platform::invalid_handle) return;
        static constexpr std::string_view kOn  =
            "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1007h";
        static constexpr std::string_view kOnHover =
            "\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[?1007h";
        static constexpr std::string_view kOff =
            "\x1b[?1007l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l";
        (void)platform::io_write_all(output_handle_,
            on ? (hover_motion_ ? kOnHover : kOn) : kOff);
        mouse_enabled_ = on;
    }

    // Does the host terminal honor DEC mode 2026 (synchronized update)?
    // Detected once at Runtime::create() via env-var heuristic; immutable
    // afterwards.
    //
    // This is the HONEST answer (env_supports_synchronized_output()), NOT
    // "did we emit the wrapper". We emit the CSI ?2026h … l wrapper on
    // every terminal by default because unknown private modes are no-ops
    // where unsupported (see emit_sync_wrapper_) — but on Apple Terminal,
    // ish, and other emulators that genuinely lack mode 2026, the wrapper
    // does nothing and multi-row repaints still tear. Applications gate
    // their ANIMATION TICK RATE on this signal: when it's false, drop the
    // caret/spinner/gradient cadence (e.g. 60 Hz → ~10 Hz) so the
    // bottom-of-frame redraw that can't be made atomic happens far less
    // often, which is the only effective flicker mitigation left on those
    // terminals. When true, the wrapper makes frames atomic and apps can
    // tick freely.
    [[nodiscard]] bool supports_synchronized_output() const noexcept {
        return sync_supported_;
    }

    // Handle a resize signal: drain, update size, mark needs_clear.
    void handle_resize();

    // Read terminal input, parse into events.
    auto read_events() -> Result<std::vector<Event>>;

    // Return unconsumed events to the front of the input stream, to be
    // delivered before any fresh bytes on the next read_events().
    //
    // The run loop uses this to end an input batch early after a navigation
    // key, so the frame that follows shows the row the user actually steered
    // to instead of only the last one in the read. Ordering is FIFO and
    // ahead of new input, which is what makes the deferral invisible: the
    // events run in the order they were typed, just one frame later.
    void push_back_events(std::vector<Event> evs) {
        if (evs.empty()) return;
        if (startup_events_.empty()) {
            startup_events_ = std::move(evs);
            return;
        }
        // Anything already queued was typed EARLIER, so it stays in front.
        startup_events_.insert(startup_events_.end(),
                               std::make_move_iterator(evs.begin()),
                               std::make_move_iterator(evs.end()));
    }

    // Drop a duplicate clipboard-read PasteEvent (the tmux OSC 5522 + OSC 52
    // double-reply case). Mutates `events` in place.
    void dedup_clipboard_pastes(std::vector<Event>& events);

    // Flush parser timeout events (e.g., bare Escape after delay).
    auto flush_timeouts() -> std::vector<Event>;

    // Render an element tree to the terminal (src/app/render.cpp), then one
    // of the two paths:
    //   render_inline      compose_inline_frame: row-diff, scrollback-preserving
    //   render_fullscreen  RenderPipeline: clear → paint → diff/serialize
    auto render(const Element& root) -> Status;
private:
    auto render_inline(const Element& root, int w) -> Status;
    auto render_fullscreen(const Element& root, int w) -> Status;
public:

    // True when this Runtime emits binary grid frames (RenderBackend::Grid)
    // instead of ANSI — a cooperating host is painting cells for us.
    [[nodiscard]] bool grid_mode() const noexcept { return grid_mode_; }

    // Pre-warm the cross-frame component cache by laying out + painting
    // `root` into a scratch canvas, WITHOUT touching the wire.
    //
    // Use case: after a heavy model swap (e.g. agentty resuming a tool-
    // heavy thread with hundreds of frozen rows), the very first
    // render() call pays the full layout + paint cost — typically tens
    // to hundreds of milliseconds for content the user has yet to see.
    // Calling warmup_render() with the same Element tree the next
    // render() will receive populates every ComponentElement with a
    // hash_id (CacheId) in the renderer's content cache; the subsequent
    // render() then takes the cell-blit fast path, dropping the visible
    // first frame to its steady-state cost.
    //
    // Constraints:
    //   — Same thread as render() (cache is thread_local).
    //   — Same StylePool (we use pool_ internally so the captured
    //     cells' style ids are valid when blit'd in render()).
    //   — Only meaningful for hash_id-keyed entries; pointer-keyed
    //     ComponentElements have ephemeral identity that won't
    //     survive the scratch → real render handoff.
    //   — Wire is NOT touched; the scratch canvas is discarded.
    //
    // Cost: roughly equal to render_tree() of `root` (layout + paint +
    // cache capture), but pays its own canvas allocation each call so
    // burn it sparingly — on the resume edge, not per-frame.
    void warmup_render(const Element& root);

    // Set terminal title via OSC 0.
    void set_title(std::string_view title);

    // Copy `text` to the system clipboard via OSC 52. Cross-platform
    // — the transport (writer_ → platform::io_write) abstracts
    // POSIX::write / Win32::WriteFile, and the escape sequence
    // itself is interpreted by the terminal emulator, not the OS.
    // Empty `text` clears the clipboard.
    //
    // Compatibility: kitty, alacritty, wezterm, foot, ghostty, rio,
    // xterm, iTerm2 (opt-in via Prefs), Windows Terminal 1.7+, tmux
    // with `set -g set-clipboard on`. Apple Terminal.app and
    // conhost.exe ignore the sequence silently — no fallback path
    // here, hosts that need guaranteed clipboard access can layer
    // xclip / wl-copy / pbcopy / clip.exe at a higher level.
    void write_clipboard(std::string_view text);

    // Emit an OSC 52 clipboard READ query. The terminal replies inline
    // (OSC 52 ; c ; <base64> ST), which InputParser decodes into a
    // PasteEvent. The portable, remote-tool-free clipboard read used by
    // the SSH image-paste path. No-op effect on terminals that don't
    // honour OSC 52 reads (no reply ever arrives — host falls back).
    void query_clipboard();

    // Emit an arbitrary, already-formed control sequence to the host
    // terminal, out-of-band with the frame renderer (see the emit_host_sequence effect
    // for the cursor-neutrality contract). Rides the SAME writer path as
    // set_title / write_clipboard — write_or_buffer, so a congested tty won't
    // drop it and the frame diff never re-emits it. The caller owns
    // well-formedness; maya writes it verbatim between frames. Empty sequence
    // is a no-op.
    void emit_host_sequence(std::string_view sequence);

    // Suspend the TUI, hand the real terminal to `fn` (an interactive
    // child process that inherits stdin/stdout/stderr), then restore the
    // TUI and force a full repaint. Blocks for the child's whole run —
    // that's intentional: the user IS interacting with the child (typing
    // a sudo password, watching output). While suspended the terminal is
    // in cooked mode with the TUI escapes torn down, so the child sees a
    // clean, line-disciplined tty. On return the next render repaints
    // from scratch (force_redraw / reset_inline as appropriate) so the
    // frame the child scrolled over is cleanly redrawn.
    //
    // No-op (fn still runs) if neither terminal is engaged — fn just runs
    // against whatever the fds currently are.
    void suspend(const std::function<void()>& fn);

    // Row count of the last composed inline frame (0 in fullscreen mode
    // or before the first render). Callers can use this as a cheap proxy
    // for tree height when deciding to virtualize. Returns 0 in any
    // inline state that doesn't carry a row count (Empty, Fresh,
    // HardReset, Sealed) — by construction these states have no
    // committed-frame row count to report.
    // Monotonic count of scrollback-invariant recoveries (committed
    // off-viewport rows + soft-repaint) since program start. Zero on a
    // healthy session; a rising value in the field is the release-build
    // signal that the single-source-of-truth invariant (maya's prev_rows
    // shadow) was violated by an upstream frame and recovered. Exposed so
    // a host / test harness can assert it stayed 0. See the member decl.
    [[nodiscard]] unsigned long scrollback_recovery_count() const noexcept {
        return scrollback_recovery_count_;
    }

    [[nodiscard]] int inline_content_rows() const noexcept {
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_))
            return s->rows();
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Stale>>(
                &in_coherence_))
            return s->rows();
        return 0;
    }

    // Legacy no-op. The inline composer anti-bounce is now FULLY
    // AUTONOMOUS inside Runtime::render (see the transient-hold block):
    // maya detects a 1-frame content dip and bridges it itself, with no
    // host policy bit. An earlier design drove the hold from this setter
    // via a Cmd, but the Cmd reliably arrived AFTER the dip window had
    // passed (and after content overflowed the viewport), so it never
    // engaged. Kept as a no-op so the old SetHeightHold plumbing and any
    // host call site remain valid. No effect.
    void set_height_hold(bool /*on*/) noexcept {}
    [[nodiscard]] int inline_min_content() const noexcept {
        return render_ctx_.inline_min_content;
    }

    // Mark prev-frame rows as committed to scrollback.
    //
    // SAFETY: the `rows` argument is a MEASURED debt from the host's
    // ScrollbackLedger::harvest() (paint-recorded rows) OR a legacy
    // host guess. The actual rows committed are
    // `min(rows, max(0, prev_rows - term_h))` — only rows that have
    // PROVABLY overflowed the current shadow's viewport are ever removed
    // from prev_cells. A caller that over-claims is clamped to the safe
    // value rather than corrupting prev_cells alignment.
    //
    // OBSERVABILITY (the type-hardening seam): the clamp is the exact
    // second-accountant signature — a row count measured against one
    // frame's geometry applied to a shadow that has since advanced
    // (harvest at frame N, this commit at frame N+k after another
    // render). It was historically SILENT, masking host↔shadow drift as
    // a benign no-op. We now COUNT every biting clamp into the release-
    // safe scrollback_recovery_count_ (so a field regression surfaces as
    // a rising metric / fails the PTY oracle's `== 0` assertion) and, in
    // debug builds, abort on it as an invariant tripwire — the host must
    // not hand maya a stale debt. A well-behaved host (ledger harvest in
    // the same update cycle that renders) never trips this: its debt is
    // exactly the rows the current shadow overflowed.
    //
    // Only meaningful when inline coherence is Synced; any other state
    // (Empty, Fresh, Stale, HardReset, Sealed) has no committed-frame
    // row count to commit against and the call is a no-op.
    void commit_inline_prefix(int rows) noexcept {
        if (rows <= 0) return;
        // Grid backend: the ANSI witness machine is inert.  Record the commit
        // so the next render_grid_frame emits a Commit frame — the host moves
        // its top `rows` grid rows into scrollback.  Return before touching
        // the inline coherence variant (which is Empty in grid mode).
        if (grid_mode_) { grid_pending_commit_ += rows; return; }
        if (!is_inline()) return;
        auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
            &in_coherence_);
        if (!s) return;
        const int term_h = std::max(1, size_.height.raw());
        const int safe_max = std::max(0, s->rows() - term_h);
        const int safe_rows = std::min(rows, safe_max);
        if (rows > safe_max) {
            // Host over-claimed against the current shadow: a stale debt.
            // Count it (release-safe, monotonic) so the drift is never
            // masked; tripwire it in debug so a mis-plumbed host is caught
            // at development time rather than in the field.
            ++scrollback_recovery_count_;
#ifndef NDEBUG
            if (std::getenv("MAYA_GATE_ABORT")) {
                std::fprintf(stderr,
                    "[maya] commit_inline_prefix: host claimed %d rows but only "
                    "%d have provably overflowed the current shadow (prev_rows=%d, "
                    "term_h=%d). This is a STALE DEBT applied to a superseded "
                    "frame -- the host harvested against one frame and committed "
                    "against another. Unset MAYA_GATE_ABORT to observe-and-clamp "
                    "instead of aborting.\n",
                    rows, safe_max, s->rows(), term_h);
                std::abort();
            }
#endif
        }
        if (safe_rows <= 0) return;
        // Move the Synced out, commit, store the new Synced back.
        // ScrollbackMarker is consumed by `commit()`; the typed token
        // guarantees `safe_rows <= prev_rows` at issue time.
        in_coherence_ = std::move(*s).commit(s->scrollback_marker(safe_rows));
        // A commit is a STRUCTURAL event that stays Synced: the next
        // frame's tree is `safe_rows` shorter (the host dropped that
        // prefix), but the render loop's bounded-clear canvas
        // preservation gates only on "prior coherence == Synced" and
        // would preserve the PRE-commit canvas prefix. When the drop
        // exceeds term_h + margin, stale rows survive BELOW the new
        // (shorter) tree's bottom, inflate content_height(), and get
        // serialized as live content — stranding the composer/status
        // chrome in native scrollback with duplicated transcript rows
        // below it. Force the next render to full-clear the canvas.
        canvas_preserve_inhibit_ = true;
    }

    // Commit every prev-frame row that has provably already overflowed
    // the viewport — i.e. `max(0, prev_rows - term_h)` rows.
    void commit_inline_overflow() noexcept {
        if (!is_inline()) return;
        auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
            &in_coherence_);
        if (!s) return;
        const int term_h = std::max(1, size_.height.raw());
        const int overflow_rows = s->rows() - term_h;
        if (overflow_rows <= 0) return;
        in_coherence_ = std::move(*s).commit(s->scrollback_marker(overflow_rows));
        // Same structural-event rule as commit_inline_prefix above:
        // the shadow shifted while coherence stays Synced, so the next
        // render must not preserve the pre-commit canvas prefix.
        canvas_preserve_inhibit_ = true;
    }

    // Force the next render to be a full repaint.
    //
    // For inline mode: demote the current Synced/Fresh state to Stale,
    // which routes the next render through compose's case (B) soft
    // redraw — cursor walks up, paints in place, erases below, no
    // scrollback wipe. The composer stays at its current viewport
    // row; host content above is preserved.
    //
    // If the inline state is already Stale or HardReset, leave it.
    //
    // Fullscreen always goes Divergent (no soft-redraw equivalent for
    // the alternate-screen-buffer model).
    void force_redraw() noexcept {
        fs_coherence_ = coherent::Divergent{};
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*s).demote_to_stale();
        }
        // This demote is CONTENT-PRESERVING: force_redraw drops wire
        // trust (ghost glyphs, foreign writes) but the canvas — the
        // app's own truth — is untouched, and the recovery repaint
        // re-serializes only the viewport window. Mark the next render
        // eligible for the frozen-prefix canvas preserve so a tall
        // transcript doesn't pay a full O(content_rows) clear+repaint
        // for a viewport-scoped fix. Recovery demotes (verify poison /
        // gate recovery) do NOT set this — their content may have
        // shifted, which is exactly why they demoted.
        canvas_preserve_stale_ok_ = true;
        // Fresh/Stale/HardReset already produce the right behavior on
        // the next render. Empty/Sealed are non-render states; ignore.
    }

    // Force the next inline render to be a HARD reset:
    // `\x1b[2J\x1b[3J\x1b[H` (clear viewport + clear saved-lines +
    // home) followed by a clean repaint from canvas row 0.
    //
    // This is the ONLY host-callable path that can reach rows the
    // application already scrolled into native scrollback. It is
    // therefore the correct recovery for a WHOLESALE CONTENT SWAP
    // into shorter content (thread switch / new thread): the old
    // thread may have overflowed the viewport, committing dozens of
    // its rows to native scrollback. Neither force_redraw (viewport-
    // only, case-B) nor commit_scrollback_overflow (advances
    // prev_rows down to term_h but leaves the physical off-viewport
    // rows on the wire) can erase those rows — they strand a copy of
    // the old transcript above the new one. demote_to_hard_reset is
    // the only coherent fix.
    //
    // Cost / caveat: \x1b[3J wipes the terminal's saved-lines
    // (native scrollback), so any pre-agentty shell history above
    // the frame is lost. That is acceptable ONLY for an explicit,
    // destructive, user-initiated content swap — never for a routine
    // frame transition (that's what force_redraw / commit-overflow
    // are for). Do NOT wire this to a per-frame or per-turn path.
    //
    // Inline only. Fullscreen already owns the whole screen via the
    // alt buffer; route it through Divergent for parity (full
    // serialize from home next frame).
    void reset_inline() noexcept {
        fs_coherence_ = coherent::Divergent{};
        if (auto* s = std::get_if<inline_frame::InlineFrame<inline_frame::Synced>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*s).demote_to_hard_reset();
        } else if (auto* st = std::get_if<inline_frame::InlineFrame<inline_frame::Stale>>(
                &in_coherence_)) {
            in_coherence_ = std::move(*st).escalate_to_hard_reset();
        }
        // Empty/Fresh/HardReset/Sealed: nothing to demote — Empty/Fresh
        // already repaint cleanly, HardReset is already armed.
    }

    // True iff the underlying writer is holding undelivered bytes from a
    // prior non-blocking write — i.e. the last render() either fully or
    // partially deferred its output because the tty pipe couldn't accept
    // it. The runtime loop must re-fire a render on the next iteration
    // (with a short poll timeout) so the deferred bytes drain rather than
    // sitting in residue indefinitely waiting for an unrelated event.
    //
    // Without this signal, render() returning ok() after a WouldBlock
    // looks identical to a fully-successful frame; the loop clears
    // needs_render and the deferred bytes never get flushed until some
    // foreground event (keystroke, stream delta, timer) happens to fire.
    // On a resize-triggered full repaint (Divergent → Synced) where the
    // emitted byte stream is large enough to saturate the writer, this
    // surfaces as the viewport scrolling partway through the new frame
    // and then sitting blank for several seconds until the next
    // unrelated event triggers a retry.
    [[nodiscard]] bool has_pending_writes() const noexcept {
        return writer_ != nullptr && writer_->has_residue();
    }

    // True iff the last render() COALESCED instead of composing — i.e. the
    // frame the caller asked for has not been painted and no bytes are
    // queued that would paint it.
    //
    // The caller must keep asking while this is true. `has_pending_writes()`
    // answers "bytes are queued"; this answers "a frame was never composed",
    // and only the pair covers every way a paint can still be owed. Without
    // it the coalesce gate silently drops one-shot visual changes (the theme
    // preview that rendered 28 KB and never reached the wire), because the
    // gate's own "the caller re-fires" assumption only holds for streams.
    [[nodiscard]] bool has_deferred_frame() const noexcept {
        // Two terms, and they answer different questions.
        //
        // coalesced_last_render_ is about the WIRE: a compose was skipped to
        // let a congested terminal catch up, so the model's current frame was
        // never built. Only the compose path knows that happened.
        //
        // owes_paint(in_coherence_) is about the SCREEN, and it is DERIVED
        // rather than enumerated. Anything other than Synced/Sealed means the
        // wire does not yet match the canvas — which is exactly what those
        // states already mean. This replaced a hand-maintained OR of
        // per-reason booleans; the reason it had to is that the list drifted
        // from the truth. A theme swap demotes Synced→Stale WITHOUT emitting
        // (demote_to_stale is a state change; the Stale arm paints on a later
        // frame), no flag said so, and a key repeat could defer that paint
        // indefinitely — the theme browser stayed one entry behind.
        //
        // Deriving means a future state cannot forget to join the list,
        // because there is no list. See owes_paint() in inline_frame.hpp.
        //
        // Gated on is_inline() because in_coherence_ is the INLINE witness
        // chain: fullscreen and grid never advance it, so it sits at its
        // default Empty for the life of the process and would report an
        // eternal debt — a busy loop rather than a missed frame. Those
        // backends carry their own coherence (fs_coherence_, grid_need_full_)
        // and re-state unconditionally when they need to.
        return coalesced_last_render_
            || (is_inline() && inline_frame::owes_paint(in_coherence_));
    }

    // True iff the input parser is holding a partial escape sequence —
    // e.g. a lone ESC byte that is either a bare Escape keypress or the
    // head of an arrow / Home / End / function-key CSI whose tail hasn't
    // arrived yet. The parser can only RESOLVE the ambiguity via
    // flush_timeout() once escape_timeout_ (50ms) elapses, and the main
    // host calls flush_timeouts() after each wait for input returns.
    //
    // Without surfacing this, an idle (fps=0) loop with no timers/spinner
    // sleeps the full 100ms idle poll while the parser sits on the ESC,
    // so a bare Escape (close picker) or a split arrow sequence (slow
    // pty / SSH / tmux delivering ESC and `[A` in separate reads) appears
    // to hang for up to 100ms before resolving. The loop consults this to
    // clamp its poll timeout to the escape deadline, mirroring the
    // has_pending_writes() → short-retry pattern above.
    [[nodiscard]] bool has_pending_input() const noexcept {
        return parser_.has_pending();
    }

    // Final cleanup (show cursor, reset, newline).
    auto cleanup() -> Status;

    // Emit the inline frame's owed restore bytes (cursor back to the
    // resting row, ?25h / ?7h / DECSCUSR / OSC-112 as claimed) and seal
    // the coherence chain. Runs in cleanup() BEFORE the Terminal dtor's
    // teardown \r\n — the hardware-caret epilogue parks the physical
    // cursor at the caret cell (above the frame bottom), and a \r\n
    // issued from there would drop the shell prompt mid-frame,
    // clobbering the remaining rows in scrollback. Idempotent.
    void finalize_inline_frame() noexcept;

    // Move-only
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

private:
    Runtime() = default;

    // -- Terminal ownership ---------------------------------------------------
    // Exactly one of these is engaged for the lifetime of the runtime.
    // Both states own their cleanup in their destructor: ~Terminal<AltScreen>
    // leaves the alt screen + restores keyboard/mouse state, and
    // ~Terminal<Inline> reverses the per-feature opt-ins (KKP,
    // modifyOtherKeys, bracketed paste, cursor) before disabling raw mode.
    // An exception escaping maya::run() therefore restores the terminal as
    // cleanly as a graceful exit — the type system enforces it.
    std::optional<Terminal<AltScreen>>  alt_terminal_;
    std::optional<Terminal<InlineMode>> inline_terminal_;
    platform::NativeHandle output_handle_ = platform::invalid_handle;
    platform::NativeHandle input_handle_  = platform::invalid_handle;

    // -- Platform signal handling ---------------------------------------------
    std::optional<platform::NativeResizeSignal> resize_signal_;

    // -- Rendering pipeline ---------------------------------------------------
    // canvas_ is the back buffer that every render paints into.  The
    // *front* canvas (fullscreen) and prev-frame state (inline) live
    // exclusively inside the corresponding `*Synced` alternative of the
    // coherence variants — they don't exist as bare members because
    // they're meaningless in the Divergent state.  This is the encoding
    // that makes "diff after a failed write" structurally inexpressible.
    std::unique_ptr<Writer>         writer_;
    StylePool                       pool_;
    Canvas                          canvas_;
    std::string                     out_;
    std::vector<layout::LayoutNode> layout_nodes_;

    // ── Grid render backend (RenderBackend::Grid) ─────────────────────────
    // A parallel output path for a cooperating host: emit the cell diff as a
    // binary grid frame instead of ANSI, WITHOUT involving the ANSI inline
    // witness machine.  We keep our own previous-frame cell snapshot and row-
    // diff canvas_ against it (the same 64-bit-packed compare the ANSI path
    // uses), so grid and ANSI never share mutable diff state.
    bool                            grid_mode_        = false;
    bool                            grid_need_full_   = true;   // full frame next
    // Set when StylePool::retheme() reports an actual theme swap; consumed by
    // the inline Synced arm to force a repaint. A theme change is invisible
    // to both wire diffs (they compare style IDS, which do not change) so it
    // has to be carried as an explicit signal rather than detected.
    bool                            retheme_repaint_  = false;
    // Sticky "a theme swap has been observed but not yet painted".
    //
    // Separate from retheme_repaint_ because the two have different
    // lifetimes: retheme_repaint_ is consumed by the inline frame arm within
    // one compose, while this survives across any number of frames that
    // return early (coalesce, backed-up wire) until one actually composes.
    // StylePool::retheme() cannot serve that role itself — it is an edge
    // detector that stores the new theme as it reports it, so the edge is
    // gone after the first call whether or not anything was painted.
    bool                            pending_retheme_  = false;
    int                             grid_prev_w_      = 0;
    int                             grid_prev_rows_   = 0;
    // Scrollback: rows the app has committed to history since the last grid
    // frame.  commit_inline_prefix() accumulates here in grid mode (the ANSI
    // witness machine is inert); render_grid_frame() emits a Commit frame for
    // them (host appends its top N rows to scrollback) and shifts its own
    // prev-cell snapshot up by N so the next diff lines up.
    int                             grid_pending_commit_ = 0;
    // True scrollback: total canvas rows PERMANENTLY committed to the host's
    // history.  The live viewport starts at this row and never re-emits below
    // it, so committed content can't duplicate.  Advances only when content
    // overflows term_h (render_grid_frame emits a Commit for the overflow).
    int                             grid_committed_rows_ = 0;
    int                             grid_prev_content_h_ = -1;   // last content_h
    std::vector<std::uint64_t>      grid_prev_cells_; // packed cells, row-major
    // Paint `root`, diff against grid_prev_cells_, emit a grid frame. Called
    // from render() when grid_mode_. Returns the same Status contract.
    auto render_grid_frame(const Element& root) -> Status;

    // Observe a theme swap and latch it until a frame actually composes.
    //
    // Must be called on EVERY render() entry, before any path that can
    // return early. StylePool::retheme() both reports the swap and consumes
    // it, so calling it late (past the coalesce gate) loses swaps whenever
    // two keypresses straddle one deferred frame.
    void note_theme_swap() {
        if (pool_.retheme()) pending_retheme_ = true;
    }
    // Initial state matters: in inline mode, defaulting to anything
    // that emits a hard-reset (\x1b[2J\x1b[3J\x1b[H) would wipe the
    // user's shell scrollback on startup. The Witness Chain's
    // `InlineFrame<Empty>` is the safe initial state — the runtime's
    // first render path seeds it to `Fresh`, which routes compose
    // through case (A): emit from the cursor's current position via
    // serialize(), growing downward without disturbing host content
    // above. Fullscreen has no equivalent concern because alt-screen
    // entry already cleared the buffer — Divergent's "home + serialize"
    // path is benign there.
    coherent::FullscreenState       fs_coherence_ = coherent::Divergent{};
    inline_frame::InlineCoherence   in_coherence_ = inline_frame::InlineFrame<inline_frame::Empty>{};

    // -- Configuration --------------------------------------------------------
    Theme         theme_              = theme::native;
    Size          size_{};
    RenderContext render_ctx_;
    uint32_t      resize_generation_  = 0;
    // Per-frame width-backstop debounce. The backstop (Runtime::render)
    // re-queries TIOCGWINSZ every frame to catch a missed SIGWINCH; but
    // some terminals (observed: kitty under certain DPI / decoration
    // states) momentarily report a width 1-2 cols off on alternating
    // queries, which without hysteresis triggers a resize storm — every
    // frame flips width, invalidates caches, and repaints (visible chrome
    // bounce). A genuine resize PERSISTS across frames, a query glitch does
    // not: only act on a width that's been observed on two consecutive
    // frames. `width_candidate_` is the last differing reading; it must
    // repeat to be accepted.
    int           width_candidate_    = 0;
    // Transient monotonic-height hold (composer anti-bounce). Maya
    // absorbs a 1-frame downward step in inline content height so the
    // composer doesn't bounce. `hold_peak_` is the running-max unpadded
    // content height while it fits the viewport; `hold_decay_` counts
    // consecutive NON-RISING frames the content has stayed below the
    // peak. After kHoldDecayFrames the shrink is treated as real and the
    // peak falls to it (pad → 0) so idle/post-settle never carries dead
    // space. Fully autonomous — no host policy bit, no Cmd race. See the
    // hold block in Runtime::render.
    //
    // kHoldDecayFrames must exceed the indicator→content handoff window:
    // the 1-row activity indicator hands off to a transient intermediate
    // height (first markdown slice not yet fully measured) that can sit
    // flat for a few frames BEFORE the real content lands and overflows.
    // Measured that gap at ~5 frames; 6 bridges it with margin. The only
    // cost of a larger value is a genuine idle shrink holding its blank
    // pad rows a few extra frames before collapsing — imperceptible.
    int           hold_peak_          = 0;
    int           hold_decay_         = 0;
    int           hold_last_unpadded_ = 0;

    // ── Adaptive wire coalescing (backpressure-driven frame batching) ──────
    //
    // On a REMOTE link (mosh / SSH / Tailscale) the wire, not the CPU, is
    // the bottleneck: a streaming turn firing ~60 RAF/sec pushes ~60 tiny
    // diff frames/sec, and each pays a fixed CUP+SGR navigation tax. The
    // wire_bytes_bench measured this as a 12.8x amplification (70 KB on the
    // wire for a 5.5 KB doc). Coalescing N appends into ONE cumulative diff
    // frame removes that per-frame overhead (identical content reaches the
    // wire) — measured 3-5x less wire.
    //
    // Runtime::render() enforces a MINIMUM interval between composes that is
    // 0 on a fast wire (zero behavior change) and grows only when the wire
    // shows backpressure. `coalesce_.congestion` is an EWMA in [0,1] of "did
    // last frame leave residue / defer": 0 = wire keeps up, 1 = saturated.
    // `coalesce_.last_compose_ms` timestamps the last frame actually
    // composed. A frame that arrives inside the congestion-scaled interval
    // is SKIPPED (coalesced): the model keeps advancing, so the next compose
    // is cumulative and strictly cheaper than the frames it replaced. A
    // deferred frame is never lost — needs_render stays set by the caller
    // and the residue-retry poll clamp re-fires it. MAYA_NO_COALESCE=1
    // disables entirely.
    // ── Adaptive wire coalescing state (see wire_coalesce.hpp) ───────────
    // The EWMA congestion estimate + last-compose timestamp that decide
    // whether to batch a streaming frame. Inert on a fast wire.
    detail::CoalesceState coalesce_{};
    std::chrono::steady_clock::time_point coalesce_epoch_{
        std::chrono::steady_clock::now()};
    // Set by render() when the coalesce gate SKIPPED a compose, cleared the
    // moment one actually happens.
    //
    // Why this is not an internal detail: a coalesced frame returns ok() and
    // emits nothing, which is byte-identical, from the caller's side, to a
    // frame that painted. The loop then clears needs_render and the frame is
    // GONE. That is fine for a streaming append (the next delta re-fires
    // within milliseconds and the diff is cumulative) and wrong for a
    // one-shot model change like a keystroke: nothing re-fires, so the paint
    // is not deferred, it is dropped.
    //
    // The residue-retry path cannot cover this case either, because the gate
    // returns BEFORE composing — there are no pending bytes for
    // has_pending_writes() to notice. So the runtime has to say so itself.
    bool coalesced_last_render_ = false;
    static constexpr int kHoldDecayFrames = 6;
    // Release-safe scrollback-invariant recovery counter. Bumped every
    // time the overflowed-frame gate (or the verify-poison arm) has to
    // COMMIT off-viewport rows + soft-repaint because prev_cells no
    // longer matches the wire — i.e. a frame that would have corrupted
    // native scrollback was caught and recovered. Zero on a healthy
    // session. Unlike the #ifndef NDEBUG abort tripwire in
    // Runtime::render (which fires on the specific committed-prefix
    // MISMATCH), this counts EVERY non-Synced recovery and survives into
    // release builds, so a field regression surfaces as a rising metric
    // in the profiler line / via scrollback_recovery_count() instead of
    // only under a debug build. Monotonic; never reset.
    unsigned long scrollback_recovery_count_ = 0;
    // One-shot inhibitor for the bounded-clear canvas preservation in
    // Runtime::render. Set by commit_inline_prefix / commit_inline_overflow
    // — the two structural events that shift the shadow while REMAINING
    // Synced, which the gate's coherence-index check cannot see. Consumed
    // (cleared) by the next render, which full-clears the canvas instead
    // of preserving a prefix that no longer matches the shortened tree.
    bool          canvas_preserve_inhibit_ = false;
    // One-shot: the pending Stale frame came from force_redraw (content
    // unchanged), so the frozen-prefix canvas preserve may fire despite
    // coherence not being Synced. Consumed by the next render pass.
    bool          canvas_preserve_stale_ok_ = false;
    // Whether to EMIT the DEC ?2026 wrapper around every frame. On by
    // default (only MAYA_NO_SYNC disables it) because unknown DEC private
    // modes are no-ops where unsupported — emitting costs ~12 bytes/frame
    // and never corrupts. The render paths read this to decide whether to
    // append sync_start/sync_end.
    //
    // The exception is TERM=dumb, and it is the DEFAULT that has to know:
    // Runtime::create() refines this flag, but the inline path a host like
    // agentty uses never runs create(), so a default of plain `true` meant
    // the wrapper was emitted no matter what TERM said. "Unknown modes are
    // no-ops" assumes a DEC private-mode parser; a dumb terminal has none,
    // so the bytes print as garbage. That is issue #37's "doesn't respect
    // TERM", and initialising from the shared predicate fixes every entry
    // point at once rather than the one that happened to be audited.
    bool          emit_sync_wrapper_  = !theme::terminal_is_dumb();

    // The HONEST env-heuristic answer to "does this terminal support mode
    // 2026". Cached at create() so we don't re-query env every frame; this
    // is what supports_synchronized_output() returns and what callers gate
    // their animation tick rate on. False on Apple Terminal / ish / plain
    // xterm / unconfigured tmux. See ansi::env_supports_synchronized_output().
    bool          sync_supported_     = false;

    // Mouse tracking was requested via Options::mouse. Emit the SGR
    // mouse-reporting enable sequence in create() and the matching disable
    // in cleanup(); cached here so cleanup() doesn't need the Options.
    bool          mouse_enabled_      = false;
    // Options::hover_motion — when set, the enable sequence also turns on
    // ANY-motion reporting (mode 1003) so bare hover (no button) produces
    // move events for hover highlights. Cached so re-enable paths match.
    bool          hover_motion_       = false;
    // Options::enhanced_keyboard — true once the kitty keyboard protocol
    // push was emitted, so cleanup()/dtor emit the matching pop on every exit
    // path (leaving it enabled would corrupt key input for the next program
    // sharing the terminal).
    bool          kitty_kbd_enabled_  = false;

public:
    /// The terminal's input handle. The host (<maya/app.hpp>) watches it with
    /// jaal's reactor and calls read_events() when it's readable. Borrowed:
    /// the Runtime keeps ownership; the handle is valid while it lives.
    [[nodiscard]] platform::NativeHandle input_handle() const noexcept { return input_handle_; }

    /// The tty's output handle: a runtime watches it for WRITABILITY
    /// while a frame is backed up, instead of polling on a timer.
    [[nodiscard]] platform::NativeHandle output_handle() const noexcept { return output_handle_; }

    /// Frame acknowledgements the parser saw since the last call (see
    /// InputParser::take_acks and maya::Screen's flow control).
    [[nodiscard]] int take_acks() noexcept { return parser_.take_acks(); }

    /// Push bytes the tty refused earlier (the tail of a frame too big for
    /// the pty buffer). True when nothing is left.
    bool drain_residue() {
        if (!writer_ || !writer_->has_residue()) return true;
        if (auto d = writer_->try_drain_residue(); !d) {
            if (d.error().kind != ErrorKind::WouldBlock) {
                writer_->discard_residue();          // hard error: repaint from scratch
                fs_coherence_ = coherent::Divergent{};
                return true;
            }
        }
        return !writer_->has_residue();
    }

private:
    // -- State ----------------------------------------------------------------
    InputParser parser_;
    bool        running_ = true;

    // Dedup guard for clipboard-read paste replies. Inside tmux we send BOTH
    // OSC 5522 (kitty image) and OSC 52 (text) because kitty is undetectable
    // there; a kitty outer terminal answers both, which would otherwise
    // surface as two paste events. Drop a PasteEvent that repeats the
    // PREVIOUS one's bytes within a short window. Content identity is what
    // separates the duplicate answer from a continuation chunk of one large
    // paste — a bare time window drops the latter too, truncating the paste
    // to its first chunk. Zero timestamp = no paste seen yet.
    std::chrono::steady_clock::time_point last_paste_at_{};
    std::string                           last_paste_content_;

    // Inline-mode mouse anchor: 1-based terminal row of the frame's top,
    // learned via a cursor-position query (DSR) at create() when mouse is
    // enabled. 0 = fullscreen or unknown (query unanswered) => no offset.
    int inline_top_row_ = 0;
    // OSC 5522 (kitty multi-format clipboard) capability, probed ONCE at
    // create() via DECRQM `CSI ? 5522 $ p` alongside the DSR query.
    //
    // Three genuinely different answers, so it is an enum. It was an int
    // whose comment had to enumerate the legal values (+1/0/-1) — which is
    // the tell that the type was never written down: nothing stopped a
    // fourth value, and `== 1` at the use site silently meant "supported"
    // while everything else collapsed into the fallback. Feature DETECTION
    // beats enumeration: a future WezTerm/Ghostty that adopts the spec is
    // discovered by asking, not by a release adding its name to a list.
    enum class Osc5522 : std::uint8_t {
        Unknown,      // no answer (query swallowed — old terminal, or a
                      // multiplexer that doesn't forward DECRQM). Fall back
                      // to env sniffing.
        Unsupported,  // terminal answered "not recognized" — definitive.
        Supported,    // answered DECRPM with set/reset: the terminal
                      // implements the 5522 family, so its clipboard READ
                      // escape is available even when the MODE is off.
    };
    Osc5522 osc5522_support_ = Osc5522::Unknown;
    int inline_frame_rows_ = 0;
    // Events that arrived interleaved with the DSR reply during create()'s
    // cursor-position query; delivered ahead of fresh input on the next
    // read_events() so a keypress in that window isn't dropped.
    std::vector<Event> startup_events_;
};

// ============================================================================
// apply_theme_canvas — the one place a theme's background becomes pixels
// ============================================================================
//
// A theme is only half-applied if its foregrounds paint but its background
// does not: every hue in a scheme was contrast-checked against THAT canvas,
// so Dracula's inks over someone's white terminal is not "Dracula", it is a
// legibility bug wearing Dracula's name. So the runtime fills the frame with
// `theme.background` — centrally, here, rather than asking each host to
// remember to wrap its own root.
//
// The decision is DATA, not a name or a table index: `owns_canvas(t)` is
// true exactly when the theme states a real background. `theme::native`
// states `default_color()` — "whatever the user's terminal already is" — so
// nothing is painted and the terminal shows through untouched. That is not a
// fallback, it is the feature: a fill would destroy terminal transparency,
// blur and background images, which is the single most-reported complaint
// against TUIs that hardcode a background.
//
// ── Why this is safe in Mode::Inline ───────────────────────────────────
//
// Fullscreen owns the screen, so a background fill is trivially sound there.
// Inline does NOT: the frame is a window onto live scrollback, and painting
// a row the frame does not own means recolouring the user's history.
//
// Two properties make it sound, and both are asserted in test_style.cpp:
//
//   1. The fill reaches the right edge. A bg-painted blank is `visible` to
//      Canvas::set (style_id != 0), so it advances that row's last-content
//      column to the full width. Without this the diff's erase-to-EOL would
//      trim the trail and you would get a ragged tear with the terminal's
//      own background showing through the gaps — the classic themed-inline
//      artifact.
//   2. The fill stops at the content. The element wraps the root's measured
//      box, so rows below max_content_row are never touched and scrollback
//      is left exactly as the user's terminal drew it.
//
// ── Cost ──────────────────────────────────────────────────────────
//
// On native (the default) this is one predicate on a Color kind and the
// element is returned untouched — no allocation, no wrap, nothing added to
// the tree. Only a theme that actually owns a canvas pays for the wrapper,
// and then it is a single Box around an existing root, not a per-cell walk.
[[nodiscard]] inline Element apply_theme_canvas(Element root, const Theme& t,
                                               int term_width) {
    if (!theme::owns_canvas(t)) return root;
    // Built directly rather than via the `| bgc()` pipe: app.hpp sits below
    // dsl.hpp in the include order, and a runtime seam should not drag the
    // whole DSL in to set one field.
    BoxElement box;
    box.layout.direction = FlexDirection::Column;
    // The TERMINAL's width, in cells — not percent(100).
    //
    // A percentage resolves against the parent's content box, and this box
    // IS the root: it has no parent to take a percentage of, so it ends up
    // sized to its own child. agentty's layout is content-sized (81 columns
    // of chrome in a 100-column window), which left a 19-column unpainted
    // stripe down the right of every row — the hard edge where the fill
    // visibly stopped. Stating the real width is the only thing that makes
    // the fill reach the edge the user can see.
    if (term_width > 0) box.layout.width = Dimension::fixed(term_width);
    // NO fill on this box, and no height either.
    //
    // A box paints its whole RECT, and this wrapper's rect comes from its
    // child — whose min_height can exceed what the host actually drew. The
    // difference got painted in the canvas colour: themed rows BELOW the
    // status bar, an inline frame colouring terminal it does not own.
    //
    // The background reaches cells two other ways, both bounded by real
    // content: build_sgr renders the canvas colour for any style that names
    // no background, and render_tree fills each PAINTED row's tail after
    // paint (where max_content_row is known). This wrapper only states the
    // frame's width.
    box.children.push_back(std::move(root));
    return Element{std::move(box)};
}

} // namespace detail

// Swap the running app's palette.
//
// Works BEFORE run<>() has published its slot, and without a Runtime at all.
// It used to no-op in that window — "a host that sets a theme during startup
// is simply ignored rather than writing through a null" — which quietly threw
// the theme away: theme::live() stayed native, and so did every palette
// PROJECTED from it (markdown's flat colours above all). Anything that built
// an Element outside a frame — startup, a headless render, a unit test — got
// the wrong palette with no indication why, and "set the theme, then start
// the UI" is the obvious order to write.
//
// Routing through the Runtime's slot is about the NO-OP GUARD below, not
// lifetime: the guard needs the previous value to compare against, and the
// Runtime owns a Theme by value that serves as it. (theme::set_live() used
// to store the pointer, which made this a lifetime requirement too; the
// live slot interns now, so a caller may hand it a temporary. The slot is
// still the right place to keep the last-set value.) When there is no
// Runtime we own one here instead, seeded to native so the first comparison
// matches theme::live()'s actual initial state.
inline void app_set_theme(const Theme& t) {
    Theme* slot = detail::Runtime::live_theme();
    if (slot == nullptr) {
        // Pre-runtime storage for the same comparison. Seeded to native so
        // "set native before startup" is correctly a no-op rather than a
        // spurious swap.
        static Theme pre_runtime = theme::native;
        slot = &pre_runtime;
    }
    // NO-OP IF UNCHANGED. Hosts resolve their theme per frame (that is
    // how `auto` follows a tmux detach or an ssh hop), so this is called
    // on every single frame with the same value almost always. A swap
    // invalidates the render cache and re-derives projected palettes, so
    // doing that unconditionally would throw away every cached component
    // 60 times a second and turn the cache into a pure cost.
    //
    // The guard lives HERE rather than in each host because it is a
    // property of what a swap costs, which is maya's knowledge, not the
    // caller's.
    if (*slot == t) return;
    *slot = t;
    // Same notification as Runtime::set_theme — this is the path hosts
    // actually use (they have no Runtime&), so a projected palette that
    // only re-derived on set_theme would never update in practice.
    detail::Runtime::on_theme_changed(*slot);
}

/// Register a callback invoked whenever the live theme is replaced.
///
/// For subsystems that keep a palette PROJECTED from the theme rather than
/// reading it per-frame (markdown's flat colour globals being the main one).
/// Without this they stay on the palette they were built with, and picking a
/// scheme repaints the chrome while prose, code and tables keep the old
/// colours — the half-themed look that makes a picker feel broken.
///
/// Call once, before the UI loop. The callback runs on the thread that
/// swapped the theme.
inline void on_theme_changed(void (*fn)(const Theme&)) {
    detail::Runtime::theme_subscribers().push_back(fn);
}

} // namespace maya
