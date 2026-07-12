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
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
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
#include <utility>
#include <variant>
#include <vector>

#include "../core/cmd.hpp"
#include "../core/concepts.hpp"
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
#include "context.hpp"
#include "quit.hpp"
#include "sub.hpp"

namespace maya {

// ============================================================================
// Ctx — render context passed to simple run() render functions
// ============================================================================

struct Ctx {
    Size  size;    ///< Current terminal dimensions
    Theme theme;   ///< Active colour theme
};

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
    bool             hover_motion = false;          ///< Also report bare (no-button) motion (mode 1003)
                                                    ///< for hover highlights. Off by default: 1003 floods
                                                    ///< move events and some terminals handle it oddly.
    Mode             mode       = Mode::Fullscreen; ///< Rendering mode
    Theme            theme      = theme::dark;      ///< Colour theme
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

// Opt-in loop-state probe (MAYA_IO_LOG=<path>). Appends to the same file the
// app.cpp poll/render trace uses; entries are timestamp-ordered so the two
// streams interleave correctly when sorted. Throttled per call site. No-op
// (one getenv) unless the env var is set. Diagnostic scaffolding.
inline void loop_dbg(std::string_view s) {
    static const char* path = std::getenv("MAYA_IO_LOG");
    if (!path) return;
    static std::FILE* f = std::fopen(path, "a");
    if (!f) return;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(f, "[%9lld] %.*s\n", static_cast<long long>(now),
                 static_cast<int>(s.size()), s.data());
    std::fflush(f);
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

// Wire the decoupled motion-framework frame-request hook to the real run
// loop. anim::Motion / Timeline / pulse (core/motion.hpp) wake the loop
// WITHOUT depending on this 90 KB header by routing through
// anim::detail::raf_hook, installed here at static-init so any TU that links
// the app gets self-driving animations for free.
namespace detail {
inline void raf_thunk_() noexcept { ::maya::request_animation_frame(); }
inline const ::maya::anim::detail::RafInstaller raf_installer_{&raf_thunk_};
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

namespace detail {
// Optional Program::visual_hash detector. When a Program type defines
// `static std::uint64_t visual_hash(const Model&)`, the run<P> loop
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
// current model, the run<P> loop performs an off-wire warmup_render of
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
    static auto create(RunConfig cfg) -> Result<Runtime>;

    void request_quit() noexcept { running_ = false; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
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

    // Poll the event source for ready flags.
    struct PollResult { bool resize = false; bool input = false; bool wake = false; };
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
    // engaged. Kept as a no-op so the Cmd::SetHeightHold plumbing and any
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
        if (!is_inline()) return;
        if (rows <= 0) return;
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
            if (!std::getenv("MAYA_NO_GATE_ABORT")) {
                std::fprintf(stderr,
                    "[maya] commit_inline_prefix: host claimed %d rows but only "
                    "%d have provably overflowed the current shadow (prev_rows=%d, "
                    "term_h=%d). This is a STALE DEBT applied to a superseded "
                    "frame -- the host harvested against one frame and committed "
                    "against another. Set MAYA_NO_GATE_ABORT=1 to observe-and-clamp "
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

    // True iff the input parser is holding a partial escape sequence —
    // e.g. a lone ESC byte that is either a bare Escape keypress or the
    // head of an arrow / Home / End / function-key CSI whose tail hasn't
    // arrived yet. The parser can only RESOLVE the ambiguity via
    // flush_timeout() once escape_timeout_ (50ms) elapses, and the main
    // loop calls flush_timeouts() once per iteration AFTER poll() returns.
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
    // An exception escaping run<P>() therefore restores the terminal as
    // cleanly as a graceful exit — the type system enforces it.
    std::optional<Terminal<AltScreen>>  alt_terminal_;
    std::optional<Terminal<InlineMode>> inline_terminal_;
    platform::NativeHandle output_handle_ = platform::invalid_handle;
    platform::NativeHandle input_handle_  = platform::invalid_handle;

    // -- Platform signal handling ---------------------------------------------
    std::optional<platform::NativeResizeSignal> resize_signal_;
    std::optional<platform::NativeEventSource>  event_source_;

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
    Theme         theme_              = theme::dark;
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
    // Whether to EMIT the DEC ?2026 wrapper around every frame. On by
    // default (only MAYA_NO_SYNC disables it) because unknown DEC private
    // modes are no-ops where unsupported — emitting costs ~12 bytes/frame
    // and never corrupts. The render paths read this to decide whether to
    // append sync_start/sync_end.
    bool          emit_sync_wrapper_  = true;

    // The HONEST env-heuristic answer to "does this terminal support mode
    // 2026". Cached at create() so we don't re-query env every frame; this
    // is what supports_synchronized_output() returns and what callers gate
    // their animation tick rate on. False on Apple Terminal / ish / plain
    // xterm / unconfigured tmux. See ansi::env_supports_synchronized_output().
    bool          sync_supported_     = false;

    // Mouse tracking was requested via RunConfig::mouse. Emit the SGR
    // mouse-reporting enable sequence in create() and the matching disable
    // in cleanup(); cached here so cleanup() doesn't need the RunConfig.
    bool          mouse_enabled_      = false;
    // RunConfig::hover_motion — when set, the enable sequence also turns on
    // ANY-motion reporting (mode 1003) so bare hover (no button) produces
    // move events for hover highlights. Cached so re-enable paths match.
    bool          hover_motion_       = false;

    // -- Wake signaling (background task → UI thread) -------------------------
    // The fd/handle is owned by BackgroundQueue: it must live as long as any
    // detached IsolatedTask thread that may try to signal it, even past the
    // runtime's lifetime. The runtime borrows it via this setter and
    // multiplexes it through event_source_'s wait(). Handle-vs-fd is hidden
    // by platform::NativeHandle (int on POSIX, HANDLE on Win32).
public:
    void set_wake_handle(platform::NativeHandle h) noexcept {
        if (event_source_) event_source_->set_wake_handle(h);
    }

private:
    // -- State ----------------------------------------------------------------
    InputParser parser_;
    bool        running_ = true;

    // Inline-mode mouse anchor: 1-based terminal row of the frame's top,
    // learned via a cursor-position query (DSR) at create() when mouse is
    // enabled. 0 = fullscreen or unknown (query unanswered) => no offset.
    int inline_top_row_ = 0;
    int inline_frame_rows_ = 0;
    // Events that arrived interleaved with the DSR reply during create()'s
    // cursor-position query; delivered ahead of fresh input on the next
    // read_events() so a keypress in that window isn't dropped.
    std::vector<Event> startup_events_;
};

} // namespace detail

// ============================================================================
// Cmd interpreter — executes effect descriptions
// ============================================================================
// The ONLY function that performs side effects from Cmd values.

namespace detail {

// Thread-safe message queue for background tasks → UI thread, plus an elastic
// worker pool for Cmd::Task execution.
//
// Lifetime model:
//   * The queue OWNS its wake mechanism (eventfd / pipe / Win32 event).
//     Detached IsolatedTask threads hold a shared_ptr; the queue (and its
//     wake fd) outlive the runtime when such a thread is still running, so
//     dispatch from a wedged task never writes to a recycled fd.
//
// Wake protocol (race-free, no producer-side atomic):
//   * send() holds mutex_, pushes the msg, and signals the wake fd ONLY when
//     it observed an empty→non-empty transition. If the queue was already
//     non-empty, the previous pusher already signaled (or will, before
//     releasing the lock). The consumer's drain() under the same lock
//     guarantees that if any message remains in messages_, exactly one wake
//     is pending in the fd. Coalescing is automatic.
//
// Pool design:
//   * Zero threads at startup; the first post() spawns a worker on demand.
//   * Workers live for the process lifetime and dequeue tasks in a loop, so
//     a read/edit/grep burst reuses a warm thread (condvar wake ~tens of µs)
//     instead of paying std::thread construction (~hundreds of µs) per call.
//   * When all workers are busy and more work arrives, a new worker is
//     spawned up to hardware_concurrency(). Long-running tasks like the SSE
//     stream therefore never starve short tool calls.
//   * workers_ is reserved to max_workers_ at construction so emplace_back
//     never reallocates — the worker_loop's `this` capture is stable, and
//     post()'s spawn path can construct the std::thread under a single lock
//     without index-tracking gymnastics.
template <typename Msg>
struct BackgroundQueue : std::enable_shared_from_this<BackgroundQueue<Msg>> {
    // ── Wake mechanism (queue-owned, lifetime-stable) ─────────────────────
    // platform::NativeWakeFd hides eventfd / pipe / NT-event differences.
    // If construction failed (rare — sandboxed eventfd, fd table full,
    // CreateEventW returned NULL) wake_.valid() is false; signal/drain
    // become safe no-ops and the runtime's unconditional drain in the
    // main loop surfaces messages with ~100ms latency. No message loss.
    platform::NativeWakeFd  wake_;

    // ── Message side (worker → UI) ────────────────────────────────────────
    std::mutex              mutex_;
    std::vector<Msg>        messages_;

    // ── Task side (UI → workers) ──────────────────────────────────────────
    // std::condition_variable_any so we can wait with a stop_token; std::jthread
    // owns the cooperative-shutdown stop_source, joins on destruction, and
    // makes the explicit shutdown flag obsolete. MoveOnlyFunction
    // avoids copy requirements that std::function imposes on captures —
    // tasks routinely move-capture unique_ptrs / move-only socket handles.
    std::mutex                                       work_mutex_;
    std::condition_variable_any                      work_cv_;
    std::deque<MoveOnlyFunction<void()>>      work_;
    std::vector<std::jthread>                        workers_;
    int                                              idle_workers_ = 0;
    unsigned                                         max_workers_ =
        std::max(4u, std::thread::hardware_concurrency());

    explicit BackgroundQueue(platform::NativeWakeFd w) noexcept
        : wake_(std::move(w))
    {
        workers_.reserve(max_workers_);
    }
    BackgroundQueue(const BackgroundQueue&) = delete;
    BackgroundQueue& operator=(const BackgroundQueue&) = delete;

    // Factory — propagates wake-fd construction failure to the caller.
    // The caller may decide to fall back to a no-wake queue (signal/drain
    // become no-ops; messages drain via the runtime's unconditional poll).
    [[nodiscard]] static auto create()
        -> Result<std::shared_ptr<BackgroundQueue>>
    {
        MAYA_TRY_DECL(auto wake, platform::NativeWakeFd::create());
        return ok(std::make_shared<BackgroundQueue>(std::move(wake)));
    }

    // Whether the wake mechanism is alive. Surface to the runtime so
    // diagnostics / event-source registration can decide how to handle
    // a degraded queue (fall back to polling drain).
    [[nodiscard]] bool wake_ok() const noexcept { return wake_.valid(); }

    // Read side handle for the runtime's event source. Returns the
    // platform's invalid sentinel (-1 / INVALID_HANDLE_VALUE) if the
    // wake fd failed to construct — event sources skip registration in
    // that case and we degrade to polling drain.
    [[nodiscard]] platform::NativeHandle wake_handle() const noexcept {
        return wake_.native_handle();
    }

    void send(Msg m) {
        bool need_signal;
        {
            std::lock_guard lk(mutex_);
            // Empty→non-empty transition: this pusher owns the wake. Anyone
            // who pushes into a non-empty queue knows the previous pusher's
            // signal is still pending in the fd (or is being emitted under
            // this lock right now), and the consumer's drain() under the
            // same lock will observe both messages atomically.
            need_signal = messages_.empty();
            messages_.push_back(std::move(m));
        }
        if (need_signal) wake_.signal();
    }

    auto drain() -> std::vector<Msg> {
        std::lock_guard lk(mutex_);
        std::vector<Msg> out;
        out.swap(messages_);
        return out;
    }

    void drain_wake() noexcept { wake_.drain(); }

    // Enqueue a task. Spawns a worker only when every existing one is busy
    // and we're still under the cap. Done under a single lock — workers_ is
    // pre-reserved so emplace_back never reallocates, and std::jthread ctor
    // throwing (e.g. EAGAIN at the system thread cap) leaves workers_
    // unchanged because vector::emplace_back has the strong-exception
    // guarantee when the move ctor is noexcept (jthread's is).
    void post(MoveOnlyFunction<void()> fn) {
        std::lock_guard lk(work_mutex_);
        work_.push_back(std::move(fn));
        if (idle_workers_ == 0 && workers_.size() < max_workers_) {
            try {
                workers_.emplace_back(
                    [this](std::stop_token stop) { worker_loop(stop); });
            } catch (const std::system_error&) {
                // Thread cap reached. Existing workers (if any) will pick
                // up the work via the notify_one below; the next post()
                // can retry the spawn. Worst case: the task waits until
                // an existing worker finishes its current job.
            }
        }
        work_cv_.notify_one();
    }

    ~BackgroundQueue() {
        // Request all workers to stop. condition_variable_any::wait with
        // a stop_token returns immediately on stop_requested(), so no
        // explicit notify_all is needed — but we send one anyway in case
        // a worker is between iterations.
        for (auto& t : workers_) t.request_stop();
        work_cv_.notify_all();
        // jthread's destructor joins automatically; clearing the vector
        // forces those joins now (rather than at our own destruction
        // order, which lets us close wake_ knowing no worker is alive).
        workers_.clear();
        // wake_'s destructor closes the underlying fd/handle. Detached
        // IsolatedTask threads each hold a shared_ptr<BackgroundQueue>,
        // so this destructor only runs once they've all exited too —
        // a wedged isolated task can't write to a recycled fd.
    }

private:
    void worker_loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            MoveOnlyFunction<void()> task;
            {
                std::unique_lock lk(work_mutex_);
                ++idle_workers_;
                // wait_v(stop_token, predicate): returns when predicate is
                // true OR stop is requested. Eliminates the manual
                // shutdown_ flag — stop_token IS the shutdown signal.
                work_cv_.wait(lk, stop, [this] { return !work_.empty(); });
                --idle_workers_;
                if (work_.empty()) return;     // stop_requested + empty
                task = std::move(work_.front());
                work_.pop_front();
            }
            // A crashing tool must not kill its worker — other tool calls
            // queued behind it would hang the UI on "Running…". Tools already
            // report their own errors via dispatch, so swallowing here is
            // safe: if we get this far it's something unexpected.
            try { task(); } catch (...) {}
        }
    }
};

// Robust steady_clock + duration addition: clamp to time_point::max() if the
// duration would overflow. C++ specifies signed-integer overflow as UB, so a
// raw `now + huge_delay` is undefined. This shows up if a caller passes
// chrono::milliseconds::max() as a "fire never" sentinel.
inline auto saturate_add(std::chrono::steady_clock::time_point t,
                         std::chrono::milliseconds            d) noexcept
    -> std::chrono::steady_clock::time_point
{
    using Tp  = std::chrono::steady_clock::time_point;
    if (d.count() < 0) return t;
    const auto headroom = Tp::max() - t;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(headroom) <= d)
        return Tp::max();
    return t + d;
}

template <typename Msg>
struct CmdContext {
    Runtime&          rt;
    std::vector<Msg>& pending;

    // TimerEntry tags each scheduled fire with the originating Sub::Every
    // interval (zero for one-shot Cmd::After). The re-arm logic in the
    // main loop matches by interval so two distinct Sub::Every
    // subscriptions don't cancel each other's re-arms (they would with a
    // pure "any timer pending" check).
    struct TimerEntry {
        std::chrono::steady_clock::time_point fire_at;
        Msg                                   msg;
        std::chrono::milliseconds             interval{0};   // 0 = one-shot
    };
    std::vector<TimerEntry>& timers;
    std::shared_ptr<BackgroundQueue<Msg>> bg_queue;
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
                saturate_add(std::chrono::steady_clock::now(), a.delay),
                a.msg,
                std::chrono::milliseconds{0},   // one-shot
            });
        },
        [&](const typename Cmd<Msg>::SetTitle& s) {
            ctx.rt.set_title(s.title);
        },
        [&](const typename Cmd<Msg>::WriteClipboard& w) {
            ctx.rt.write_clipboard(w.content);
        },
        [&](const typename Cmd<Msg>::QueryClipboard&) {
            ctx.rt.query_clipboard();
        },
        [&](const typename Cmd<Msg>::Task& t) {
            if (ctx.bg_queue) {
                // shared_ptr keeps the queue alive even if run<P>() exits early
                auto q = ctx.bg_queue;
                auto task_fn = t.run;
                q->post([q, task_fn = std::move(task_fn)] {
                    task_fn([q](Msg m) { q->send(std::move(m)); });
                });
            } else {
                // Fallback: synchronous (for simple run() without bg support)
                t.run([&](Msg m) { ctx.pending.push_back(std::move(m)); });
            }
        },
        [&](const typename Cmd<Msg>::IsolatedTask& t) {
            // Dedicated thread per call. The runtime never owns the thread —
            // it's detached at construction so a wedged syscall (slow NFS,
            // dead FUSE mount, hung subprocess) leaks one thread instead of
            // permanently consuming a slot in the shared BG worker pool.
            // The shared_ptr<BackgroundQueue> capture keeps the dispatch
            // sink alive even if the runtime tears down before the thread
            // finishes — Msg send becomes a no-op once the queue's wake fd
            // is closed during shutdown, but the captured shared_ptr still
            // holds the queue object so the lambda can run to completion.
            //
            // Without a bg_queue, we can't dispatch from another thread
            // safely. Fall back to the inline path the regular Task uses.
            if (ctx.bg_queue) {
                auto q = ctx.bg_queue;
                auto task_fn = t.run;
                try {
                    std::thread([q, task_fn = std::move(task_fn)] {
                        task_fn([q](Msg m) { q->send(std::move(m)); });
                    }).detach();
                } catch (const std::system_error&) {
                    // pthread_create returned EAGAIN (per-process or system
                    // thread cap reached). Degrade gracefully: run the task
                    // on the shared pool. Worst case the shared pool is also
                    // saturated and the user sees the existing queueing
                    // behaviour — strictly better than dropping the work.
                    auto qq = ctx.bg_queue;
                    auto fn = t.run;
                    qq->post([qq, fn = std::move(fn)] {
                        fn([qq](Msg m) { qq->send(std::move(m)); });
                    });
                }
            } else {
                t.run([&](Msg m) { ctx.pending.push_back(std::move(m)); });
            }
        },
        [&](const typename Cmd<Msg>::CommitScrollback& c) {
            ctx.rt.commit_inline_prefix(c.rows);
        },
        [&](const typename Cmd<Msg>::CommitScrollbackOverflow&) {
            ctx.rt.commit_inline_overflow();
        },
        [&](const typename Cmd<Msg>::ForceRedraw&) {
            ctx.rt.force_redraw();
        },
        [&](const typename Cmd<Msg>::ResetInline&) {
            ctx.rt.reset_inline();
        },
        [&](const typename Cmd<Msg>::SetHeightHold& s) {
            ctx.rt.set_height_hold(s.on);
        },
        [&](const typename Cmd<Msg>::Suspend& s) {
            // Synchronous by design — the user is interacting with the
            // child (typing a sudo password, watching output); the UI
            // thread has nothing else to do. The runtime handles the
            // teardown/restore + re-anchor; we push the child's result
            // Msg into pending so the very next drain folds it into the
            // model before the post-suspend repaint.
            if (!s.run) return;
            Msg result = [&] {
                std::optional<Msg> out;
                ctx.rt.suspend([&] { out.emplace(s.run()); });
                // s.run() throwing propagates out of rt.suspend after the
                // tty is restored — out stays empty only if that happens,
                // and the throw skips this lambda's return anyway. The
                // fallback default-constructed Msg is unreachable in
                // practice but keeps the expression total.
                return out ? std::move(*out) : Msg{};
            }();
            ctx.pending.push_back(std::move(result));
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

    // Background task queue — tasks spawned by Cmd::task run on worker
    // threads and deliver messages here. The queue OWNS its wake
    // mechanism (platform::NativeWakeFd: eventfd / pipe / NT event); we
    // plug the read-side native handle into the runtime's event_source
    // so poll() wakes on dispatch. shared_ptr ensures the queue (and
    // its wake handle) outlive any detached IsolatedTask thread that
    // captured it, so a wedged task's late dispatch never writes to a
    // recycled fd.
    //
    // If wake-fd construction failed (rare — sandboxed eventfd, fd
    // table full, CreateEventW returned NULL), fall back to a queue
    // with an invalid wake handle. The runtime's main loop drains the
    // queue every iteration regardless of the wake flag, so messages
    // surface with ~100ms latency instead of being event-driven —
    // never lost.
    std::shared_ptr<detail::BackgroundQueue<Msg>> bg_queue;
    if (auto bgq = detail::BackgroundQueue<Msg>::create()) {
        bg_queue = std::move(*bgq);
    } else {
        // Construct a queue with a no-op wake. Tasks still dispatch
        // synchronously through send(); the runtime polls.
        bg_queue = std::make_shared<detail::BackgroundQueue<Msg>>(
            platform::NativeWakeFd{});
    }
    rt.set_wake_handle(bg_queue->wake_handle());

    // Interpreter bookkeeping (not part of Model — these are runtime state)
    std::vector<Msg> pending_msgs;
    std::vector<typename detail::CmdContext<Msg>::TimerEntry> timers;
    detail::CmdContext<Msg> ctx{rt, pending_msgs, timers, bg_queue};

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

    // Visual-hash gate state. Populated only when Program defines
    // visual_hash(Model); ignored otherwise.
    std::uint64_t last_visual_hash       = 0;
    bool          last_visual_hash_valid = false;

    // Edge-detector for the optional Program::needs_warmup hook. When
    // P::needs_warmup(model) rises from false to true we fire one
    // warmup_render(); leaving the flag at true on subsequent frames
    // does NOT re-warm (programs that don't actively clear the flag
    // still only pay the warmup once per state change).
    bool          last_warmup_done       = false;

    // Frame-request scheduler. A widget that called
    // request_animation_frame() during the last render sets
    // detail::animation_requested_; we then schedule the next repaint
    // for last-render + kAnimationFrameInterval. This is the SINGLE
    // animation clock — no separate deadline global, no parallel
    // AnimationFrame pump. When no widget requests a frame, this stays
    // in the past and the loop idles. `unset` sentinel = no pending
    // frame.
    auto next_frame_at = std::chrono::steady_clock::time_point{};

    // ── Main event loop ──────────────────────────────────────────────────
    while (rt.is_running()) {
        if (detail::mouse_request >= 0) {
            rt.apply_mouse(detail::mouse_request != 0);
            detail::mouse_request = -1;
        }
        // Compute poll timeout: min of 100ms, fps frame time, nearest timer.
        // Zero on first frame so the UI appears immediately (Windows CMD
        // has no initial event to wake the poll — without this the first
        // render is delayed by the full timeout).
        auto poll_timeout = needs_render
            ? std::chrono::milliseconds(0)
            : std::chrono::milliseconds(100);
        if (!needs_render && cfg.fps > 0) {
            poll_timeout = std::chrono::milliseconds(1000 / std::max(1, cfg.fps));
        }
        // Deferred-write retry: if the last render() couldn't push its
        // bytes to the wire (WouldBlock), the writer is sitting on
        // residue and needs a re-fire to drain it. Bound the poll
        // timeout to a single-digit-millisecond retry interval so the
        // residue clears within a couple of loop iterations rather
        // than waiting for an unrelated event to come along. Surfaces
        // as the resize-triggered full repaint resuming smoothly
        // instead of pausing for seconds when the tty buffer saturates.
        if (rt.has_pending_writes()) {
            poll_timeout = std::min(poll_timeout,
                                    std::chrono::milliseconds(8));
        }
        // Pending-escape retry: the parser is holding a partial escape
        // sequence (lone ESC, or the head of an arrow/Home/End CSI). Only
        // flush_timeout() can resolve it, and only after the 50ms escape
        // deadline. Clamp the poll so the loop wakes to call
        // flush_timeouts() shortly after the deadline instead of sleeping
        // the full 100ms idle timeout — otherwise a bare Escape or a
        // split arrow key (slow pty / SSH / tmux) stalls for up to 100ms
        // before the keystroke registers. Most visible in the pickers,
        // which idle with no spinner tick to keep the loop spinning.
        if (rt.has_pending_input()) {
            poll_timeout = std::min(poll_timeout,
                                    std::chrono::milliseconds(16));
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
        // Frame request: a widget called request_animation_frame()
        // during its last build(), so a repaint is scheduled at
        // next_frame_at. Cap the poll so the loop wakes in time to
        // service it. Same path the Sub::Every timers use — one wake
        // computation, one clock.
        if (!needs_render
            && next_frame_at != std::chrono::steady_clock::time_point{})
        {
            auto now = std::chrono::steady_clock::now();
            auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_frame_at - now);
            poll_timeout = std::min(poll_timeout,
                std::max(std::chrono::milliseconds(0), until));
        }

        // Loop-state probe (throttled). Captures WHY poll_timeout is what it
        // is when the loop is hot-spinning (timeout==0). MAYA_IO_LOG only.
        {
            static std::chrono::steady_clock::time_point last_dbg{};
            const auto now = std::chrono::steady_clock::now();
            if (now - last_dbg >= std::chrono::milliseconds(25)) {
                last_dbg = now;
                long long nfa = -1;
                if (next_frame_at != std::chrono::steady_clock::time_point{})
                    nfa = std::chrono::duration_cast<std::chrono::milliseconds>(
                              next_frame_at - now).count();
                long long ntimer = -1;
                for (auto& t : timers) {
                    auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 t.fire_at - now).count();
                    if (ntimer < 0 || d < ntimer) ntimer = d;
                }
                detail::loop_dbg(std::format(
                    "LOOP pt={} nr={} nfa={} ntimer={} timers={} pend={} animreq={} hpw={}",
                    poll_timeout.count(), needs_render ? 1 : 0, nfa, ntimer,
                    timers.size(), pending_msgs.size(),
                    detail::animation_requested_ ? 1 : 0,
                    rt.has_pending_writes() ? 1 : 0));
            }
        }

        // Wait for events
        auto poll_result = rt.poll(poll_timeout);
        if (!poll_result) break;

        // Handle resize — coalesce rapid events (e.g. window drag)
        if (poll_result->resize) {
            rt.handle_resize();
            // Drain any further pending resizes before rendering
            while (auto more = rt.poll(std::chrono::milliseconds(0))) {
                if (!more->resize) break;
                rt.handle_resize();
            }
            auto sz = rt.size();
            ResizeEvent re{sz.width, sz.height};
            Event ev{re};
            detail::dispatch_through_sub(current_sub, ev, pending_msgs);
            // Force a render — apps that don't subscribe to ResizeEvent
            // (the common case) still need their view re-laid-out at the
            // new size. Without this, with fps=0 the canvas dimensions
            // update but the next paint waits for an unrelated event.
            needs_render = true;
            // Width changed — layout invariants the visual hash doesn't
            // capture have moved. Force the next render to run even if
            // the model hash matches.
            last_visual_hash_valid = false;
        }

        // Read and dispatch terminal input
        if (poll_result->input) {
            auto events = rt.read_events();
            if (!events) break;
            for (auto& ev : *events) {
                if (const int dy = rt.inline_mouse_dy(); dy > 0) {
                    if (auto* me = std::get_if<MouseEvent>(&ev)) {
                        const int fr = me->y.value - dy;
                        const int fh = rt.inline_frame_rows();
                        if (fh > 0 && (fr < 1 || fr > fh)) continue;
                        me->y = Rows{fr};
                    }
                }
                // Auto-dispatch to scroll states painted in the
                // previous frame (default behavior — opt out per
                // state with auto_dispatch = false).
                for (auto* s : detail::live_scroll_states()) {
                    if (s && s->auto_dispatch) (void)s->handle_event(ev);
                }
                detail::dispatch_through_sub(current_sub, ev, pending_msgs);
            }
        }

        // Drain background task messages.
        //
        // The wake protocol normally guarantees: if any send() pushed a
        // message, exactly one wake is pending in the fd until consumed.
        // Drain the fd FIRST so additional sends after drain() will signal
        // again (their empty→non-empty check sees the queue empty
        // post-swap).
        //
        // We drain `messages_` unconditionally — not gated on
        // poll_result->wake — so that if the wake mechanism is broken
        // (`bg_queue->wake_ok() == false`, e.g. eventfd blocked by
        // seccomp, fd table full, or NT event create failed) the
        // 100ms idle timeout still surfaces the messages. Worst case is
        // event-driven dispatch degrades to polling-style ~100ms
        // latency; messages are never lost. The mutex acquire is ~30ns
        // when uncontested.
        if (poll_result->wake) bg_queue->drain_wake();
        for (auto& m : bg_queue->drain())
            pending_msgs.push_back(std::move(m));

        // Flush parser timeouts (e.g., bare Escape)
        for (auto& ev : rt.flush_timeouts()) {
            if (const int dy = rt.inline_mouse_dy(); dy > 0) {
                if (auto* me = std::get_if<MouseEvent>(&ev)) {
                    const int fr = me->y.value - dy;
                    const int fh = rt.inline_frame_rows();
                    if (fh > 0 && (fr < 1 || fr > fh)) continue;
                    me->y = Rows{fr};
                }
            }
            for (auto* s : detail::live_scroll_states()) {
                if (s && s->auto_dispatch) (void)s->handle_event(ev);
            }
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

        // Frame request: a widget asked for another paint by calling
        // request_animation_frame() during the last render. If its
        // scheduled frame time has arrived, repaint — the widget's next
        // build() either re-requests (animation continues) or doesn't
        // (next_frame_at is cleared below → loop returns to idle).
        if (next_frame_at != std::chrono::steady_clock::time_point{}
            && std::chrono::steady_clock::now() >= next_frame_at)
        {
            needs_render = true;
        }

        // Reconcile Sub::Every timers EVERY iteration — not gated on
        // needs_render. Invariant: each Sub::Every interval present in
        // the current subscription must have exactly one armed timer at
        // all times. Gating this behind the render block created a
        // freeze-until-keypress hole: if the model became active (e.g. a
        // background-queue message kicked a new turn) on an iteration
        // that did NOT also set needs_render, the Tick timer was never
        // armed, the loop fell back to the 100 ms idle poll, and — since
        // nothing re-armed it — the spinner/stream sat frozen until an
        // unrelated event (a keypress) forced a render that finally
        // reconciled. Rebuilding the sub here keeps the timer set
        // honest against the live model on every pass. Cheap: a Sub
        // rebuild + small vector scan, no allocation when steady.
        current_sub = get_sub();
        {
            auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<std::chrono::milliseconds, Msg>> timer_specs;
            detail::collect_timers(current_sub, timer_specs);
            for (auto& [interval, msg] : timer_specs) {
                if (interval.count() <= 0) continue;   // ill-formed sub
                bool already_pending = false;
                for (auto& t : timers) {
                    if (t.interval == interval) { already_pending = true; break; }
                }
                if (!already_pending) {
                    timers.push_back({
                        detail::saturate_add(now, interval),
                        std::move(msg),
                        interval,
                    });
                }
            }
        }

        if (needs_render) {
            // Rebuild subscriptions from current model
            current_sub = get_sub();

            // (Timer reconciliation moved above the gate — see the
            // unconditional reconcile block. Each Sub::Every entry needs
            // exactly one timer scheduled at all times, independent of
            // whether this iteration renders.)

            // Optional visual-hash gate. When the Program provides
            // visual_hash(Model), skip view()+render() if the hash
            // matches the last rendered frame's. Eliminates the
            // full layout+paint walk on Tick wakeups whose deltas
            // didn't change anything the user can see.
            //
            // The hash should bucket time-driven animations (cursor
            // blink phase, spinner frame, streaming caret pulse) so
            // each visible step advances the hash. RAF without an
            // accompanying hash advance means the widget's visual
            // wants to step at a finer cadence than visual_hash
            // captures — the framework can't tell whether to render,
            // and conservatively the program should include a coarse
            // time bucket in its hash for any RAF-driven visual.
            bool skip_render = false;
            if constexpr (detail::HasVisualHash<P>::value) {
                const std::uint64_t h = P::visual_hash(model);
                if (h == last_visual_hash && last_visual_hash_valid) {
                    skip_render = true;
                } else {
                    last_visual_hash       = h;
                    last_visual_hash_valid = true;
                }
            }
            // Backpressure overrides the gate. In inline mode the ONLY
            // residue drainer is rt.render() (its first act is
            // try_drain_residue; on a congested tty it defers the frame
            // entirely). If the writer is still holding bytes the wire
            // rejected last frame, skipping render() because the visual
            // hash matched would strand that residue until some unrelated
            // axis (the next Tick bucket, ~33ms out) flips the hash — the
            // frame appears "stuck" while the deferred bytes sit
            // undrained, then snaps in late. Force the render so each
            // loop iteration makes drain progress; the poll-timeout clamp
            // above bounds the retry cadence so this can't busy-spin.
            if (rt.has_pending_writes()) skip_render = false;

            // RAF override — the structural guarantee that an animation
            // can NEVER be stranded by a visual-hash coverage gap.
            //
            // A widget that called request_animation_frame() during the
            // last build() is explicitly asking for another paint. If we
            // skip because the program's visual_hash didn't advance (its
            // time bucket didn't cover this widget's finer cadence), the
            // widget never gets to run build() again, so it can't advance
            // its own wall-clock state OR re-request — the hash stays
            // frozen and we skip FOREVER. That's the "md reveal / spinner
            // gets stuck until a keypress" class of bug, and it recurs
            // every time a new RAF-driven visual is added without a
            // matching hash term.
            //
            // Honour the pending RAF directly instead of trusting every
            // program to bucket time perfectly: if a frame was requested,
            // render. `animation_requested_` still holds the LAST render's
            // request here (it's cleared just below, only when we don't
            // skip), so this reads "did the previous frame ask for
            // another?" Cost when steady-animating is one render per RAF
            // interval (16 ms) — exactly what the animation wanted — and
            // the flag clears the instant a build() stops re-requesting,
            // so a settled UI still idles at zero renders.
            if (detail::animation_requested_) skip_render = false;

            // Clear the per-render frame-request flag ONLY when we are
            // about to actually run view() — build() is what re-sets it.
            // A skipped render (visual hash matched) does NOT clear it,
            // because a skip means "nothing visible changed at this
            // instant," not "the animation ended": the widget never got
            // a chance to re-request. Clearing on skip is exactly what
            // froze RAF animations until a keypress.
            if (!skip_render) detail::animation_requested_ = false;
            // Reset the delay policy in lockstep with the flag: build()
            // re-sets both, a skip clears neither (build() didn't run, so
            // the latched policy must ride to the next scheduling). -1 =
            // "unset" so the next frame's first requester establishes it.
            if (!skip_render) detail::next_frame_delay_ms_ = -1;

            if (!skip_render) {
                // Pure: view(model) → Element → render to terminal
                Element view_root = P::view(model);
                // Optional one-shot warmup: if the program flagged the
                // current model as needing a cache pre-warm (e.g. a
                // heavy thread just rehydrated), paint the same view
                // into a scratch canvas first so the wire-bound render
                // takes the cell-blit fast path. Single-render-equivalent
                // CPU spent off-wire, in exchange for converting the
                // user-visible first frame from O(content) to O(blit).
                //
                // De-duped via `last_warmup_done`: the same flag stays
                // true across reducer steps until something clears it,
                // but we only fire warmup_render on the rising edge so
                // a program that never clears the flag still only pays
                // one warmup per swap.
                if constexpr (detail::HasNeedsWarmup<P>::value) {
                    const bool want = P::needs_warmup(model);
                    if (want && !last_warmup_done) {
                        rt.warmup_render(view_root);
                    }
                    last_warmup_done = want;
                }
                auto status = rt.render(view_root);
                if (!status) break;
            }
            needs_render = false;

            // Schedule the next frame. One clock: a frame is just
            // "wake again in kAnimationFrameInterval." The schedule is
            // kept alive as long as a widget is requesting frames.
            //
            //   - render ran + widget re-requested  → schedule next.
            //   - render ran + NO request            → animation settled,
            //                                          clear the schedule.
            //   - render SKIPPED (hash matched)      → KEEP the existing
            //     schedule. A skip can't observe a request (build()
            //     didn't run), so treating it as "no request" would
            //     cancel a live animation; instead we let the schedule
            //     ride so the loop wakes next interval, the visual-hash
            //     time bucket advances, the render runs, and build()
            //     re-arms. This is what keeps spinner / caret / sigil
            //     animating without a keypress.
            if (!skip_render) {
                if (detail::animation_requested_) {
                    // Honour a widget's requested minimum delay (the slow
                    // caret blink asks for its ~265 ms half-period) so the
                    // loop sleeps between visible steps instead of waking at
                    // 60 fps. 0 (the common case) falls back to the default
                    // ~16 ms interval used by fast animations.
                    const auto delay = detail::next_frame_delay_ms_ > 0
                        ? std::chrono::milliseconds(detail::next_frame_delay_ms_)
                        : detail::kAnimationFrameInterval;
                    next_frame_at = std::chrono::steady_clock::now() + delay;
                } else {
                    next_frame_at = std::chrono::steady_clock::time_point{};
                }
            } else if (next_frame_at != std::chrono::steady_clock::time_point{}) {
                // Keep riding: advance to the next interval from now so
                // the loop doesn't busy-spin re-detecting an already-due
                // frame it just chose to skip. A skip can't observe a fresh
                // delay request (build() didn't run), so use the last one
                // still latched in next_frame_delay_ms_ (not cleared on a
                // skip, mirroring animation_requested_).
                const auto delay = detail::next_frame_delay_ms_ > 0
                    ? std::chrono::milliseconds(detail::next_frame_delay_ms_)
                    : detail::kAnimationFrameInterval;
                next_frame_at = std::chrono::steady_clock::now() + delay;
            }
            // Same scroll-writeback re-render as the simple run() path:
            // if a ScrollState's max_* changed during this paint, the
            // current view used stale zeros. Re-render so the next view
            // sees the fresh values.
            if (detail::scroll_writeback_dirty) {
                detail::scroll_writeback_dirty = false;
                needs_render = true;
                last_visual_hash_valid = false;   // force next render
            }
            // Deferred-write retry: if render() returned ok() but the
            // writer is still holding residue (the tty pipe rejected
            // part of the emit with WouldBlock — common on a
            // resize-triggered full repaint that emits a large
            // serialize stream), keep needs_render set so the next
            // loop iteration drives the drain. Paired with the
            // poll-timeout cap above, this finishes the frame within
            // a few milliseconds instead of stalling for seconds.
            if (rt.has_pending_writes()) needs_render = true;
        }
    }

    (void)rt.cleanup();
}

// ============================================================================
// run(cfg, event_fn, render_fn) — convenience wrapper for simple apps
// ============================================================================
// For quick prototypes and simple tools where the full Program ceremony
// is overkill. Wraps closures into the Runtime loop directly.
//
//   run(
//       {.title = "demo"},
//       [&](const Event& ev) { return !key(ev, 'q'); },
//       [&] { return text("hello") | Bold; }
//   );
//
// Event function: (const Event&) -> bool  (false = quit)
//            or:  (const Event&) -> void  (call maya::quit() to exit)
// Render function: () -> Element
//             or:  (const Ctx&) -> Element

template <typename F>
concept SimpleEventFn =
    (std::invocable<F, const Event&> &&
     std::convertible_to<std::invoke_result_t<F, const Event&>, bool>) ||
    (std::invocable<F, const Event&> &&
     std::is_void_v<std::invoke_result_t<F, const Event&>>);

template <typename F>
concept SimpleRenderFn =
    (std::invocable<F> &&
     std::convertible_to<std::invoke_result_t<F>, Element>) ||
    (std::invocable<F, const Ctx&> &&
     std::convertible_to<std::invoke_result_t<F, const Ctx&>, Element>);

template <SimpleEventFn EventFn, SimpleRenderFn RenderFn>
void run(RunConfig cfg, EventFn&& event_fn, RenderFn&& render_fn) {
    auto result = detail::Runtime::create(cfg);
    if (!result) {
        auto msg = std::format("maya: failed to initialize terminal: {}\n",
                               result.error().message);
        std::fputs(msg.c_str(), stderr);
        return;
    }
    auto rt = std::move(*result);
    detail::quit_requested = false;
    detail::mouse_request = -1;

    const auto base_timeout = cfg.fps > 0
        ? std::chrono::milliseconds(1000 / std::max(1, cfg.fps))
        : std::chrono::milliseconds(100);

    bool needs_render = true;

    // Frame pacing for continuous (fps>0) apps. Without this the loop renders
    // flat-out: cfg.fps only sized base_timeout, but needs_render was forced
    // true every iteration so poll_timeout collapsed to 0 and we spun far
    // faster than fps — burning CPU and FLOODING the terminal with frames
    // (which then keep draining after the user quits, making quit feel slow).
    // We now gate the per-frame needs_render flip on the frame deadline and
    // sleep the poll until then, so an fps=N app renders at ~N fps and the
    // output backlog stays small.
    using PaceClock = std::chrono::steady_clock;
    const auto frame_interval = cfg.fps > 0
        ? std::chrono::nanoseconds(1'000'000'000LL / std::max(1, cfg.fps))
        : std::chrono::nanoseconds(0);
    auto next_frame = PaceClock::now();

    // The last view() result, kept so a residue-drain re-render can re-enter
    // rt.render() WITHOUT recomputing the user's view. maya's compose path
    // ignores the passed element while residue is pending (it drains and
    // returns early), so this only needs to be a valid Element to satisfy
    // the signature.
    Element last_root;   // default = empty TextElement; only used to satisfy
    bool    have_root = false;   // rt.render()'s signature during a residue drain

    auto dispatch = [&](const Event& ev_in) {
        // Inline mode: translate absolute SGR mouse row into a frame-relative
        // row before hit-testing, so scroll states, the user's event_fn, and
        // mouse_pos() all line up with the rendered UI. No-op when fullscreen.
        Event ev = ev_in;
        if (const int dy = rt.inline_mouse_dy(); dy > 0) {
            if (auto* me = std::get_if<MouseEvent>(&ev)) {
                const int fr = me->y.value - dy;          // frame-relative row
                const int fh = rt.inline_frame_rows();
                if (fh > 0 && (fr < 1 || fr > fh)) return; // outside the frame: ignore
                me->y = Rows{fr};
            }
        }
        // Auto-dispatch to scroll states that were painted in the
        // previous frame. Any state with auto_dispatch = true (default)
        // gets the event before the user's event_fn — so scrollbars
        // and viewport content "just work" without per-app boilerplate.
        // The framework consumes the result; we still pass the event
        // through to the user so they can layer their own behavior.
        for (auto* s : detail::live_scroll_states()) {
            if (s && s->auto_dispatch) (void)s->handle_event(ev);
        }
        if constexpr (std::is_void_v<std::invoke_result_t<EventFn, const Event&>>) {
            event_fn(ev);
            if (detail::quit_requested) rt.request_quit();
        } else {
            if (!event_fn(ev)) rt.request_quit();
        }
        // Mirror Program<P> (see needs_render flip after Sub dispatch above):
        // event-driven apps using plain widget state (no Signal, no Program)
        // must still repaint after input. The SIMD diff makes the cost of a
        // potentially-unchanged frame negligible (zero terminal writes if no
        // cells changed) — far cheaper than the alternative of silently
        // swallowing user input.
        needs_render = true;
    };

    while (rt.is_running()) {
        // Apply a pending runtime mouse toggle (maya::set_mouse).
        if (detail::mouse_request >= 0) {
            rt.apply_mouse(detail::mouse_request != 0);
            detail::mouse_request = -1;
        }
        // Zero timeout when a render is pending so the first frame appears
        // immediately (critical on Windows CMD where no initial event wakes
        // the poll).
        auto poll_timeout = needs_render
            ? std::chrono::milliseconds(0) : base_timeout;
        // Frame-pacing clamp: for a continuous app that has nothing else
        // pending, sleep the poll until the next frame deadline instead of
        // spinning. Input still wakes the poll immediately (the OS wait
        // returns on readable fd), so responsiveness is unaffected.
        if (cfg.fps > 0 && !needs_render) {
            auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_frame - PaceClock::now());
            if (until.count() < 0) until = std::chrono::milliseconds(0);
            poll_timeout = until;
        }
        // Deferred-write retry (mirrors the Program<P> loop, app.hpp ~1172):
        // the output fd is non-blocking, so a large frame can leave part of
        // its bytes in the writer's residue. Bound the poll to a few ms so
        // the residue drains on its own instead of waiting for the next
        // input event — without this a backpressured first frame paints only
        // partially and fills in one chunk per keypress.
        if (rt.has_pending_writes()) {
            poll_timeout = std::min(poll_timeout, std::chrono::milliseconds(8));
        }
        // Pending-escape retry (mirrors the Program<P> loop): the parser
        // holds a partial escape sequence that only flush_timeout() can
        // resolve, and only after the 50ms escape deadline. Clamp the poll
        // so the loop wakes to flush it promptly instead of sleeping the
        // full idle timeout.
        if (rt.has_pending_input()) {
            poll_timeout = std::min(poll_timeout, std::chrono::milliseconds(16));
        }
        auto poll_result = rt.poll(poll_timeout);
        if (!poll_result) break;

        if (poll_result->resize) {
            rt.handle_resize();
            // Coalesce rapid resizes (e.g. window drag) before rendering
            while (auto more = rt.poll(std::chrono::milliseconds(0))) {
                if (!more->resize) break;
                rt.handle_resize();
            }
            needs_render = true;
        }

        if (poll_result->input) {
            auto events = rt.read_events();
            if (!events) break;
            for (auto& ev : *events) dispatch(ev);
        }

        for (auto& ev : rt.flush_timeouts()) dispatch(ev);

        // Continuous-render apps repaint each frame — but only once the
        // frame deadline has arrived, so we pace at cfg.fps instead of
        // spinning. Input-driven repaints (dispatch sets needs_render) are
        // unaffected and still paint immediately.
        if (cfg.fps > 0 && PaceClock::now() >= next_frame) {
            needs_render = true;
            next_frame += frame_interval;
            // If we fell far behind (terminal was slow / app was paused),
            // don't try to "catch up" by rendering a burst — reset the
            // deadline to now so we resume clean single-frame cadence.
            if (next_frame < PaceClock::now())
                next_frame = PaceClock::now() + frame_interval;
        }

        if (needs_render) {
            Element root = [&]() -> Element {
                if constexpr (std::invocable<RenderFn, const Ctx&>) {
                    Ctx ctx{rt.size(), rt.theme()};
                    return render_fn(ctx);
                } else {
                    return render_fn();
                }
            }();
            auto status = rt.render(root);
            if (!status) break;
            last_root = root;
            have_root = true;
            needs_render = false;
            // If a ScrollState's max_* changed during this paint, the
            // view function we just ran read stale zeros for them.
            // Schedule an immediate second render so the next frame's
            // status text and clamping logic see the fresh values.
            if (detail::scroll_writeback_dirty) {
                detail::scroll_writeback_dirty = false;
                needs_render = true;
            }
        }

        // Residue drain — SEPARATE from a full re-render. If the tty rejected
        // part of a large emit (WouldBlock), the unwritten tail sits in the
        // writer's residue. We must finish shipping it, but we must NOT
        // re-run render_fn() (the user's view) to do so: that recomputes the
        // whole frame every loop iteration for a heavy app, pinning the CPU
        // and rendering far above cfg.fps (a flood that the terminal then
        // can't composite, causing visible tearing). Instead, re-enter
        // rt.render() with the SAME last frame — maya's compose path detects
        // pending residue and just drains it (it does not recompose), so this
        // is cheap and the per-frame view cost is paid once per real frame.
        if (rt.has_pending_writes() && have_root) {
            (void)rt.render(last_root);
            // Keep the poll short (clamped above to 8ms) so the drain
            // finishes promptly without spinning the view.
        }
    }

    detail::quit_requested = false;
    (void)rt.cleanup();
}

template <SimpleEventFn EventFn, SimpleRenderFn RenderFn>
void run(EventFn&& event_fn, RenderFn&& render_fn) {
    run(RunConfig{}, std::forward<EventFn>(event_fn), std::forward<RenderFn>(render_fn));
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
