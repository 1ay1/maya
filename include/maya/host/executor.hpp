#pragma once
// maya/host/executor.hpp — maya's view-side parallelism, run on jaal.
//
// libmaya can't link a runtime, so its two parallel spots (pixel rows, the
// big markdown parse) go through maya::exec (core/executor.hpp). This is the
// jaal side: maya::run installs it for the program's lifetime and joins
// everything it started before returning.

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>

#include "../core/executor.hpp"

namespace maya::host_detail {

class executor_scope {
  public:
    executor_scope() {
        exec::install(exec::Executor{
            // Called from view() code (Pixels::fill_rows) on the loop thread.
            // A view is a pure function of the model: it holds no jaal lock
            // and can't reach one (a guarded needs a capability, and view()
            // is never handed one). So the loop thread is unlocked here, and
            // the join below is a wait it may make. This is the one reviewed
            // root for maya's view-side parallelism (lint: ban_cap_mint).
            .parallel_for = [](std::size_t n,
                               const std::function<void(std::size_t)>& fn) {
                if (n <= 1) { if (n) fn(0); return; }
                ::jaal::thread_root([&](const ::jaal::unlocked& cap) {
                    ::jaal::scope(cap, [&](::jaal::nursery& nur) {
                        for (std::size_t i = 1; i < n; ++i)
                            nur.spawn([&fn, i] { fn(i); });
                        fn(0);   // the caller does one share itself
                    });
                });
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
        });
    }
    executor_scope(const executor_scope&)            = delete;
    executor_scope& operator=(const executor_scope&) = delete;

  private:
    std::unique_ptr<::jaal::kernel::pool> pool_ =
        std::make_unique<::jaal::kernel::pool>(/*max_workers=*/2);
};

}  // namespace maya::host_detail
