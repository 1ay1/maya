// test_sub_batch_routing — the per-event re-route contract of the Program
// loop's input path.
//
// Regression for "input fails to reach sometimes": a fast terminal (or
// paste, or key repeat) delivers several key events in ONE read_events()
// batch. The loop used to route the ENTIRE batch through the subscription
// snapshot taken before the batch — so a key that changed the model (opened
// a picker, closed a modal) left the rest of the batch routed by a STALE
// subscription: keys landed in the surface that was open before, or
// nowhere. The fix drains + rebuilds the sub between events.
//
// The run loop needs a real terminal, so this pins the CONTRACT at the
// same seam the loop uses: dispatch_through_sub + a model-dependent
// subscribe(), driven exactly like the loop drives them — once WITHOUT the
// inter-event drain (documenting the failure) and once WITH it (the fix).
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <maya/app/sub.hpp>
#include <maya/app/app.hpp>

#include "agtest.hpp"

namespace {

using maya::CharKey;
using maya::Event;
using maya::KeyEvent;

// A two-mode app: 'p' opens the "picker"; while open, letters type into
// the picker's query. The model-dependent subscription is the point.
struct M {
    bool picker_open = false;
    std::string query;          // typed while open
    std::string composer;       // typed while closed
};
struct OpenPicker {};
struct QueryChar { char c; };
struct ComposerChar { char c; };
using Msg = std::variant<OpenPicker, QueryChar, ComposerChar>;

maya::Sub<Msg> subscribe(const M& m) {
    return maya::Sub<Msg>::on_key([open = m.picker_open](const KeyEvent& k)
                                      -> std::optional<Msg> {
        const auto* ch = std::get_if<CharKey>(&k.key);
        if (!ch) return std::nullopt;
        const char c = static_cast<char>(ch->codepoint);
        if (!open && c == 'p') return OpenPicker{};
        if (open) return QueryChar{c};
        return ComposerChar{c};
    });
}

void update(M& m, const Msg& msg) {
    std::visit(maya::overload{
        [&](const OpenPicker&)    { m.picker_open = true; },
        [&](const QueryChar& q)   { m.query += q.c; },
        [&](const ComposerChar& c){ m.composer += c.c; },
    }, msg);
}

std::vector<Event> batch_pab() {
    // One read: "p a b" — p opens the picker, a/b must land in its query.
    return {Event{KeyEvent{CharKey{U'p'}}},
            Event{KeyEvent{CharKey{U'a'}}},
            Event{KeyEvent{CharKey{U'b'}}}};
}

} // namespace

TEST_CASE("sub batch routing: stale snapshot misroutes (the documented bug)") {
    M m;
    auto sub = subscribe(m);
    std::vector<Msg> pending;
    // The OLD loop shape: whole batch through one snapshot, drain at the end.
    for (const auto& ev : batch_pab())
        maya::detail::dispatch_through_sub(sub, ev, pending);
    for (const auto& msg : pending) update(m, msg);

    CHECK(m.picker_open);
    // The bug, pinned: a/b went to the COMPOSER (routed by the stale
    // pre-'p' subscription), not the picker query.
    CHECK(m.query == "");
    CHECK(m.composer == "ab");
}

TEST_CASE("sub batch routing: per-event re-route delivers to the new surface") {
    M m;
    auto sub = subscribe(m);
    std::vector<Msg> pending;
    // The FIXED loop shape: drain + rebuild the sub between events.
    for (const auto& ev : batch_pab()) {
        maya::detail::dispatch_through_sub(sub, ev, pending);
        if (!pending.empty()) {
            for (const auto& msg : pending) update(m, msg);
            pending.clear();
            sub = subscribe(m);
        }
    }

    CHECK(m.picker_open);
    CHECK(m.query == "ab");
    CHECK(m.composer == "");
}
