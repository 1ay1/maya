#pragma once
// maya/host/executor.hpp — maya's view-side parallelism, run on jaal.
//
// libmaya can't link a runtime, so its two parallel spots (pixel rows, the
// big markdown parse) go through maya::exec (core/executor.hpp). This is the
// jaal side: maya::run installs it for the program's lifetime and joins
// everything it started before returning.

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>

#include "../core/executor.hpp"

namespace maya::host_detail {

// One parallel_for call. Workers and the caller claim indices from `next`;
// the caller returns once `done` reaches n. A helper that starts after all
// indices are claimed touches only this block (co-owned), never fn.
struct fork_join {
    const std::function<void(std::size_t)>* fn;
    std::size_t                             n;
    std::atomic<std::size_t>                next{0};
    std::atomic<std::size_t>                done{0};
    std::atomic<bool>                       failed{false};
    std::exception_ptr                      error;   // first throw, set once by whoever flips failed

    void drain() {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            try {
                (*fn)(i);
            } catch (...) {
                if (!failed.exchange(true, std::memory_order_acq_rel)) error = std::current_exception();
            }
            if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == n) done.notify_one();
        }
    }
};

class executor_scope {
  public:
    executor_scope() {
        exec::install(exec::Executor{
            // Called from view() code (Pixels::fill_rows) on the loop thread,
            // every frame. Helpers are warm pool workers, not a thread each
            // call; the caller drains indices too, so it never waits on a
            // helper that hasn't started.
            .parallel_for = [p = par_.get()](std::size_t n,
                                             const std::function<void(std::size_t)>& fn) {
                if (n <= 1) { if (n) fn(0); return; }
                auto job = std::make_shared<fork_join>();
                job->fn = &fn;
                job->n  = n;
                for (std::size_t i = 1; i < n; ++i)
                    ::jaal::kernel::pool_access::post(
                        *p, [job](std::stop_token) { job->drain(); });
                job->drain();
                for (std::size_t d = job->done.load(std::memory_order_acquire); d < n;
                     d = job->done.load(std::memory_order_acquire))
                    job->done.wait(d, std::memory_order_acquire);
                if (job->error) std::rethrow_exception(job->error);
            },
            .post = [p = pool_.get()](std::function<void()> job) {
                // Already erased by maya::exec, so it goes in through the
                // kernel's key rather than the checked post().
                ::jaal::kernel::pool_access::post(
                    *p, [job = std::move(job)](std::stop_token) { job(); });
            },
        });
    }
    ~executor_scope() {
        exec::install({});
        // Every background parse has finished (or never starts) by here.
        // maya::run's frame, after jaal::run returned: no lock is held.
        ::jaal::thread_root([&](const ::jaal::unlocked& cap) {
            (void)pool_->shutdown(cap, ::jaal::kernel::pool::no_deadline);
            (void)par_->shutdown(cap, ::jaal::kernel::pool::no_deadline);
        });
    }
    executor_scope(const executor_scope&)            = delete;
    executor_scope& operator=(const executor_scope&) = delete;

  private:
    std::unique_ptr<::jaal::kernel::pool> pool_ =
        std::make_unique<::jaal::kernel::pool>(/*max_workers=*/2);
    // parallel_for helpers: started lazily, one per core, kept warm.
    std::unique_ptr<::jaal::kernel::pool> par_ = std::make_unique<::jaal::kernel::pool>(
        std::max(1u, std::thread::hardware_concurrency()));
};

}  // namespace maya::host_detail
