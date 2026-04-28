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

// platform detection must be visible before the POSIX header gate below;
// MAYA_PLATFORM_WIN32 is defined inside platform/detect.hpp.
#include "../platform/detect.hpp"

#if !MAYA_PLATFORM_WIN32
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif
#endif

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
namespace coherent {

struct FullscreenSynced {
    Canvas front;   // canonical "what the terminal currently displays"
};

struct InlineSynced {
    InlineFrameState state;
};

struct Divergent {};

using FullscreenState = std::variant<FullscreenSynced, Divergent>;
using InlineState     = std::variant<InlineSynced,     Divergent>;

} // namespace coherent

class Runtime {
public:
    static auto create(RunConfig cfg) -> Result<Runtime>;

    void request_quit() noexcept { running_ = false; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] Size size() const noexcept { return size_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] bool is_inline() const noexcept { return inline_terminal_.has_value(); }

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

    // Set terminal title via OSC 0.
    void set_title(std::string_view title);

    // Row count of the last composed inline frame (0 in fullscreen mode
    // or before the first render).  Callers can use this as a cheap proxy
    // for tree height when deciding to virtualize.  Returns 0 in the
    // Divergent state (the prev_rows is part of InlineFrameState which
    // only exists inside InlineSynced — by construction).
    [[nodiscard]] int inline_content_rows() const noexcept {
        if (auto* s = std::get_if<coherent::InlineSynced>(&in_coherence_))
            return s->state.prev_rows;
        return 0;
    }

    // Mark the top `rows` rows of the current prev frame as committed to
    // scrollback so the next frame can render a shorter tree without the
    // renderer interpreting it as "content removed from the bottom".  No
    // effect in fullscreen mode or when render coherence is Divergent
    // (there is no prev-frame state to commit against — the next render
    // will be a full repaint anyway).
    void commit_inline_prefix(int rows) noexcept {
        if (!is_inline()) return;
        if (auto* s = std::get_if<coherent::InlineSynced>(&in_coherence_))
            s->state.commit_prefix(rows);
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
    // Initial state matters: in inline mode, Divergent means "previous
    // content from us is on the terminal, must erase before re-painting"
    // — but on the very first frame nothing of ours has been drawn, so
    // the terminal IS the user's shell.  Defaulting to Divergent would
    // emit \x1b[2J\x1b[3J\x1b[H and erase their scrollback.  Defaulting
    // to InlineSynced{} (empty InlineFrameState) means "we know what
    // we've drawn: nothing", so compose_inline_frame paints from the
    // cursor downward without disturbing what's above.  Fullscreen has
    // no equivalent concern because alt-screen entry already cleared
    // the buffer — Divergent's "home + serialize" path is benign there.
    coherent::FullscreenState       fs_coherence_ = coherent::Divergent{};
    coherent::InlineState           in_coherence_ = coherent::InlineSynced{};

    // -- Configuration --------------------------------------------------------
    Theme         theme_              = theme::dark;
    Size          size_{};
    RenderContext render_ctx_;
    uint32_t      resize_generation_  = 0;

    // -- Wake signaling (background task → UI thread) -------------------------
    // The fd/handle is owned by BackgroundQueue: it must live as long as any
    // detached IsolatedTask thread that may try to signal it, even past the
    // runtime's lifetime. The runtime borrows it via these setters and
    // multiplexes it through event_source_'s wait().
public:
#if MAYA_PLATFORM_WIN32
    void set_wake_handle(platform::NativeHandle h) noexcept {
        if (event_source_) event_source_->set_wake_handle(h);
    }
#else
    void set_wake_fd(int fd) noexcept {
        if (event_source_) event_source_->set_wake_fd(fd);
    }
#endif

private:
    // -- State ----------------------------------------------------------------
    InputParser parser_;
    bool        running_ = true;
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
#if MAYA_PLATFORM_WIN32
    platform::NativeHandle  wake_handle_ = platform::invalid_handle;
#elif defined(__linux__)
    int                     wake_fd_ = -1;   // eventfd: read==write
#else
    int                     wake_read_fd_  = -1;
    int                     wake_write_fd_ = -1;
#endif

    // ── Message side (worker → UI) ────────────────────────────────────────
    std::mutex              mutex_;
    std::vector<Msg>        messages_;

    // ── Task side (UI → workers) ──────────────────────────────────────────
    std::mutex                          work_mutex_;
    std::condition_variable             work_cv_;
    std::deque<std::function<void()>>   work_;
    std::vector<std::thread>            workers_;
    int                                 idle_workers_ = 0;
    bool                                shutdown_ = false;
    unsigned                            max_workers_ =
        std::max(4u, std::thread::hardware_concurrency());

    BackgroundQueue() {
        workers_.reserve(max_workers_);

#if MAYA_PLATFORM_WIN32
        wake_handle_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
#elif defined(__linux__)
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
        int fds[2];
        if (::pipe(fds) == 0) {
            for (int fd : fds) {
                int flags = ::fcntl(fd, F_GETFL);
                if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            wake_read_fd_  = fds[0];
            wake_write_fd_ = fds[1];
        }
#endif
    }
    BackgroundQueue(const BackgroundQueue&) = delete;
    BackgroundQueue& operator=(const BackgroundQueue&) = delete;

    // Read side accessors for the runtime's event_source.
#if MAYA_PLATFORM_WIN32
    [[nodiscard]] platform::NativeHandle wake_handle() const noexcept {
        return wake_handle_;
    }
#else
    [[nodiscard]] int wake_read_fd() const noexcept {
#ifdef __linux__
        return wake_fd_;
#else
        return wake_read_fd_;
#endif
    }
#endif

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
        if (need_signal) signal_wake();
    }

    auto drain() -> std::vector<Msg> {
        std::lock_guard lk(mutex_);
        std::vector<Msg> out;
        out.swap(messages_);
        return out;
    }

    void drain_wake() noexcept {
#if MAYA_PLATFORM_WIN32
        if (wake_handle_ != platform::invalid_handle)
            ::ResetEvent(wake_handle_);
#elif defined(__linux__)
        if (wake_fd_ >= 0) {
            uint64_t val;
            (void)::read(wake_fd_, &val, sizeof(val));
        }
#else
        if (wake_read_fd_ < 0) return;
        char buf[4096];
        while (::read(wake_read_fd_, buf, sizeof(buf)) > 0) {}
#endif
    }

    // Enqueue a task. Spawns a worker only when every existing one is busy
    // and we're still under the cap. Done under a single lock — workers_ is
    // pre-reserved so emplace_back never reallocates, and std::thread ctor
    // throwing (e.g. EAGAIN at the system thread cap) leaves workers_
    // unchanged because the strong-exception guarantee of vector::emplace_back
    // rolls back the slot on throw.
    void post(std::function<void()> fn) {
        std::lock_guard lk(work_mutex_);
        work_.push_back(std::move(fn));
        if (idle_workers_ == 0 && workers_.size() < max_workers_) {
            try {
                workers_.emplace_back([this] { worker_loop(); });
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
        {
            std::lock_guard lk(work_mutex_);
            shutdown_ = true;
        }
        work_cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        // Safe to close after join — workers may have called signal_wake
        // during their final task, but those writes complete before the
        // worker exits, and join() blocks until exit.
#if MAYA_PLATFORM_WIN32
        if (wake_handle_ != platform::invalid_handle)
            ::CloseHandle(wake_handle_);
#elif defined(__linux__)
        if (wake_fd_ >= 0) ::close(wake_fd_);
#else
        if (wake_read_fd_  >= 0) ::close(wake_read_fd_);
        if (wake_write_fd_ >= 0) ::close(wake_write_fd_);
#endif
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lk(work_mutex_);
                ++idle_workers_;
                work_cv_.wait(lk, [this] { return !work_.empty() || shutdown_; });
                --idle_workers_;
                if (work_.empty() && shutdown_) return;
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

    void signal_wake() noexcept {
#if MAYA_PLATFORM_WIN32
        if (wake_handle_ != platform::invalid_handle)
            ::SetEvent(wake_handle_);
#elif defined(__linux__)
        if (wake_fd_ < 0) return;
        uint64_t val = 1;
        (void)::write(wake_fd_, &val, sizeof(val));
#else
        if (wake_write_fd_ < 0) return;
        char byte = 1;
        (void)::write(wake_write_fd_, &byte, 1);
#endif
    }
};

template <typename Msg>
struct CmdContext {
    Runtime&          rt;
    std::vector<Msg>& pending;

    struct TimerEntry {
        std::chrono::steady_clock::time_point fire_at;
        Msg                                    msg;
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

    // Background task queue — tasks spawned by Cmd::task run on worker threads
    // and deliver messages here. The queue OWNS its wake mechanism; we plug
    // the read side into the runtime's event_source so poll() wakes on
    // dispatch. shared_ptr ensures the queue (and its wake fd) outlive any
    // detached IsolatedTask thread that captured it, so a wedged task's
    // late dispatch never writes to a recycled fd.
    auto bg_queue = std::make_shared<detail::BackgroundQueue<Msg>>();
#if MAYA_PLATFORM_WIN32
    rt.set_wake_handle(bg_queue->wake_handle());
#else
    rt.set_wake_fd(bg_queue->wake_read_fd());
#endif

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

    // ── Main event loop ──────────────────────────────────────────────────
    while (rt.is_running()) {
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
        }

        // Read and dispatch terminal input
        if (poll_result->input) {
            auto events = rt.read_events();
            if (!events) break;
            for (auto& ev : *events) {
                detail::dispatch_through_sub(current_sub, ev, pending_msgs);
            }
        }

        // Drain background task messages.
        // The wake protocol guarantees: if any send() pushed a message,
        // exactly one wake is pending in the fd until consumed. Drain the
        // fd first so additional sends after drain() will signal again
        // (their empty→non-empty check sees the queue empty post-swap).
        if (poll_result->wake) {
            bg_queue->drain_wake();
            for (auto& m : bg_queue->drain())
                pending_msgs.push_back(std::move(m));
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

    const auto base_timeout = cfg.fps > 0
        ? std::chrono::milliseconds(1000 / std::max(1, cfg.fps))
        : std::chrono::milliseconds(100);

    bool needs_render = true;

    auto dispatch = [&](const Event& ev) {
        if constexpr (std::is_void_v<std::invoke_result_t<EventFn, const Event&>>) {
            event_fn(ev);
            if (detail::quit_requested) rt.request_quit();
        } else {
            if (!event_fn(ev)) rt.request_quit();
        }
    };

    while (rt.is_running()) {
        // Zero timeout when a render is pending so the first frame appears
        // immediately (critical on Windows CMD where no initial event wakes
        // the poll).
        auto poll_timeout = needs_render
            ? std::chrono::milliseconds(0) : base_timeout;
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

        if (cfg.fps > 0) needs_render = true;

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
            needs_render = false;
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
