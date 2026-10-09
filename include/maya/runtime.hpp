#pragma once
// maya/runtime.hpp — the runtime, as an app sees it.
//
// An app depends on maya and never on jaal: agentty → maya → jaal, strictly.
// Everything an app needs from the runtime — the program types, effects,
// subscriptions, tasks, locks, cancellation, processes, polling, test hosts —
// is here under a maya:: name. They are ALIASES of jaal's, not copies: no
// code, no cost, and a jaal type and its maya name are the same type.
//
// Why a seam at all, when the names are the same: it makes jaal maya's
// implementation rather than the app's dependency. An app that includes
// only <maya/...> can't grow a second runtime by accident, and maya is free
// to reshape what it exposes. When an app needs a jaal piece that isn't
// here, it is added here — never reached around.
//
//   #include <maya/runtime.hpp>
//
//   using Cmd = maya::Cmd<Msg, my_effect>;
//   maya::guarded<State> st;
//   maya::scope([&](maya::nursery& n) { ... });
//
// Extension points (sendable_opt_in, frozen_opt_in) are templates declared
// in jaal; a specialisation has to name the template where it was declared.
// Use the macros at the bottom so app code still never spells jaal.

#include <jaal/jaal.hpp>
#include <jaal/core/co_owned.hpp>
#include <jaal/core/sync.hpp>
#include <jaal/kernel/delay.hpp>
#include <jaal/kernel/loop.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/published.hpp>
#include <jaal/kernel/stop_group.hpp>
#include <jaal/kernel/worker_group.hpp>
#include <jaal/platform/clock.hpp>
#include <jaal/platform/concepts.hpp>
#include <jaal/platform/process.hpp>
#include <jaal/platform/signal.hpp>
#include <jaal/platform/select.hpp>
#if !defined(_WIN32)
#  include <jaal/platform/posix/process.hpp>
#  include <jaal/platform/posix/poll_reactor.hpp>
#endif

namespace maya {

// ── The program: model, messages, effects, subscriptions ────────────────
template <class Msg, class... Effects>
using Cmd = ::jaal::Cmd<Msg, Effects...>;
template <class Msg, class... Sources>
using Sub = ::jaal::Sub<Msg, Sources...>;
template <class Msg>
using Sink = ::jaal::Sink<Msg>;

using ::jaal::pure_fx;          // declare an app effect: pure_fx<Payload, "name">
using ::jaal::payload_t;
using ::jaal::router;           // declare an event source
using ::jaal::host_context;
using ::jaal::require_host_for;
using ::jaal::run_options;
using ::jaal::Subscribing;
using ::jaal::HasVisualHash;
using ::jaal::HasNeedsWarmup;
using ::jaal::handled_as_group;
namespace fx = ::jaal::fx;      // core effects: fx::quit, …
// The bare runtime concept (maya::Program, in host/sources.hpp, adds view()).
template <class P>
concept RuntimeProgram = ::jaal::Program<P>;
// What a host hook sees: fd readiness and delivered signals.
using ::jaal::readiness;
using ::jaal::sig;

// ── Values that cross threads ───────────────────────────────────────────
using ::jaal::Sendable;
using ::jaal::Frozen;
using ::jaal::Sync;             // many threads may use one T& at once
using ::jaal::shared;           // Sendable && Frozen, shared without a lock
using ::jaal::co_owned;         // Sync state that jobs and their owner share

// ── Concurrency ─────────────────────────────────────────────────────────
using ::jaal::guarded;          // a value only reachable under its lock
using ::jaal::scope;            // structured helpers: all joined on return
using ::jaal::nursery;
using pool       = ::jaal::kernel::pool;        // owned worker threads
using stop_group = ::jaal::kernel::stop_group;  // cancel or await work
using worker_group = ::jaal::kernel::worker_group;  // owned jobs, barrier stop
using ::jaal::kernel::CheckedJob;               // captureless body, Sendable args
using ::jaal::kernel::require_job;
using ::jaal::kernel::delay_for;                // a sleep that wakes on stop
using ::jaal::kernel::loop_bound;               // state only the loop touches
using ::jaal::kernel::published;                // the current object, swapped whole
using ::jaal::kernel::loop_identity;
using ::jaal::kernel::on_loop;          // is this thread a kernel's loop?
using ::jaal::kernel::loop_token;

// ── Platform: processes, polling, locks, the clock ──────────────────────
namespace platform {
using ::jaal::platform::native_file_lock;
using ::jaal::platform::steady_clock;
// A child process and the vocabulary for running one.
using ::jaal::platform::process_spec;
using ::jaal::platform::exit_status;
using ::jaal::platform::stop_mode;
using ::jaal::platform::stop_scope;
using ::jaal::platform::stream_to;
using ::jaal::platform::interest;
using ::jaal::platform::readiness;
using ::jaal::platform::wait_result;
#if !defined(_WIN32)
using ::jaal::platform::posix_process;
using ::jaal::platform::poll_reactor;
#else
using ::jaal::platform::windows_process;
using ::jaal::platform::read_some;
using ::jaal::platform::write_some;
using ::jaal::platform::quote_arg;
using ::jaal::platform::join_command_line;
using ::jaal::platform::cmd_command_line;
using ::jaal::platform::resolves_to_batch;
#endif
using ::jaal::platform::native_process;   // posix_process or windows_process
using ::jaal::platform::native_reactor;
using ::jaal::borrowed_handle;
using ::jaal::owned_handle;
using ::jaal::platform::duplicate_handle;
}  // namespace platform

// ── Test hosts: run a program without a terminal ────────────────────────
using ::jaal::sim;
using ::jaal::sim_options;
using ::jaal::headless;
using ::jaal::explore;

}  // namespace maya

// Opt a type into Sendable / Frozen when jaal can't see inside it. These
// specialise jaal's templates (the only place a specialisation may live)
// without the app naming jaal.
//
//   MAYA_SENDABLE(agentty::LazyBytes);                        // one type
//   template <class Tag> MAYA_SENDABLE_T(agentty::Id<Tag>);   // a family
#define MAYA_SENDABLE(...) \
    template <> inline constexpr bool ::jaal::sendable_opt_in<__VA_ARGS__> = true
#define MAYA_FROZEN(...) \
    template <> inline constexpr bool ::jaal::frozen_opt_in<__VA_ARGS__> = true
#define MAYA_SENDABLE_T(...) \
    inline constexpr bool ::jaal::sendable_opt_in<__VA_ARGS__> = true
#define MAYA_FROZEN_T(...) \
    inline constexpr bool ::jaal::frozen_opt_in<__VA_ARGS__> = true
// A class whose every member function is safe to call concurrently.
#define MAYA_SYNC(...) \
    template <> inline constexpr bool ::jaal::sync_opt_in<__VA_ARGS__> = true
