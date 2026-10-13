// maya::exec — see include/maya/core/executor.hpp.
#include "maya/core/executor.hpp"

#include <mutex>
#include <utility>

namespace maya::exec {

namespace {
// Written once by maya::run before the program starts and cleared after it
// ends, read by widgets during view(). The lock makes the install/clear and a
// concurrent read safe; it is held only to copy the functions out.
std::mutex g_mu;
Executor   g_exec;

Executor current() {
    std::lock_guard lk(g_mu);
    return g_exec;
}
}  // namespace

void install(Executor e) {
    std::lock_guard lk(g_mu);
    g_exec = std::move(e);
}

void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    auto e = current();
    if (e.parallel_for) { e.parallel_for(n, fn); return; }
    for (std::size_t i = 0; i < n; ++i) fn(i);   // no runtime: run inline
}

void post(std::function<void()> job) {
    auto e = current();
    if (e.post) { e.post(std::move(job)); return; }
    job();                                        // no runtime: run inline
}

}  // namespace maya::exec
