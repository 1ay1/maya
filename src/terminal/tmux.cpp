// maya::tmux — impl. See the header for the capability model and the
// measured tmux behaviours that motivate it.

#include "maya/terminal/tmux.hpp"
#include "maya/terminal/ansi.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace maya::tmux {

namespace {

// ── One-shot probe of the live tmux server ───────────────────────────
// A `tmux display-message -p` round-trip costs ~8 ms; we pay it ONCE per
// process and cache. Every accessor funnels through probe().
struct Probe {
    bool        in_tmux      = false;
    bool        passthrough  = false;
    std::string features;      // raw comma list from #{client_termfeatures}
};

std::string env_str(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string{v} : std::string{};
}

// Presence: $TMUX (tmux on THIS host) or a tmux-*/screen-* $TERM that
// survived an ssh hop (tmux on the LOCAL side). Kept identical to the
// long-standing ansi::tmux_in_path() so behaviour can't drift between
// the two — that function now delegates here.
bool detect_presence() {
    if (!env_str("TMUX").empty()) return true;
    const std::string term = env_str("TERM");
    return term.rfind("tmux", 0) == 0 || term.rfind("screen", 0) == 0;
}

// Ask the running server. Returns "" on any failure (no server, tmux not
// on PATH, permission) — callers treat that as "unknown", never as a
// hard error: a capability probe must not be able to break rendering.
std::string ask_tmux(const char* format) {
    std::string cmd = "tmux display-message -p '";
    cmd += format;
    cmd += "' 2>/dev/null";
    // NOLINTNEXTLINE(cert-env33-c) — fixed, argument-free command string;
    // `format` is a compile-time literal from THIS file, never user input.
    std::FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// Cache-valid flags. Single-threaded by contract for the reset seam (see
// the header): production sets them once on first use and never clears.
bool& probe_done()  { static bool d = false; return d; }
bool& sync_done()   { static bool d = false; return d; }

Probe& probe() {
    static Probe p;
    if (!probe_done()) {
        probe_done() = true;
        p = Probe{};
        p.in_tmux = detect_presence();
        if (p.in_tmux) {
            // Only meaningful when a server is actually reachable: the
            // ssh-local topology ($TERM says tmux, but the server lives
            // on the OTHER host) yields empty answers, which correctly
            // degrade to "no features, no passthrough".
            p.features = ask_tmux("#{client_termfeatures}");
            const std::string pt =
                ask_tmux("#{?#{==:#{allow-passthrough},on},1,"
                         "#{?#{==:#{allow-passthrough},all},1,0}}");
            p.passthrough = (pt == "1");
        }
    }
    return p;
}

[[nodiscard]] std::string_view feature_name(Feature f) noexcept {
    switch (f) {
        case Feature::Sync:         return "sync";
        case Feature::CursorStyle:  return "cstyle";
        case Feature::CursorColour: return "ccolour";
        case Feature::Focus:        return "focus";
        case Feature::Clipboard:    return "clipboard";
        case Feature::ExtendedKeys: return "extkeys";
        case Feature::Rgb:          return "RGB";
        case Feature::Hyperlinks:   return "hyperlinks";
        case Feature::Title:        return "title";
    }
    return {};
}

// Exact, comma-delimited membership test (so "sync" never matches a
// hypothetical "nosync", and "RGB" is case-sensitive as tmux spells it).
bool list_contains(std::string_view list, std::string_view want) {
    if (want.empty()) return false;
    std::size_t i = 0;
    while (i <= list.size()) {
        const std::size_t j = list.find(',', i);
        const std::size_t end = (j == std::string_view::npos) ? list.size() : j;
        if (list.substr(i, end - i) == want) return true;
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return false;
}

// Resolved sync markers — see the header for why "no passthrough inside
// tmux" means NO sync rather than a raw attempt.
struct SyncPair { std::string begin, end; };

const SyncPair& sync_pair() {
    static SyncPair sp;
    if (!sync_done()) {
        sync_done() = true;
        sp = SyncPair{};
        const std::string raw_begin{ansi::sync_start};
        const std::string raw_end{ansi::sync_end};
        if (!active()) {                       // no tmux: plain markers
            sp.begin = raw_begin;
            sp.end   = raw_end;
        } else if (passthrough_allowed()) {    // tmux: must be wrapped
            sp.begin = wrap(raw_begin);
            sp.end   = wrap(raw_end);
        }
        // else — tmux without passthrough: a raw marker is swallowed by
        // tmux (measured) and a wrapped one is dropped. Emit nothing.
    }
    return sp;
}

} // namespace

bool active() noexcept { return probe().in_tmux; }

bool passthrough_allowed() noexcept { return probe().passthrough; }

bool has_feature(Feature f) noexcept {
    const Probe& p = probe();
    if (!p.in_tmux) return false;
    return list_contains(p.features, feature_name(f));
}

std::string wrap(std::string_view seq) {
    if (!active()) return std::string{seq};
    std::string out;
    out.reserve(seq.size() * 2 + 16);
    out += "\x1bPtmux;";
    for (char c : seq) {
        out.push_back(c);
        if (c == '\x1b') out.push_back('\x1b');   // ESC must be DOUBLED
    }
    out += "\x1b\\";
    return out;
}

std::string_view sync_begin() noexcept { return sync_pair().begin; }
std::string_view sync_end()   noexcept { return sync_pair().end; }
bool sync_available() noexcept { return !sync_pair().begin.empty(); }

void reset_cache_for_test() noexcept {
    probe_done() = false;
    sync_done()  = false;
}

} // namespace maya::tmux
