// test_tmux_capabilities — the tmux capability layer (maya::tmux).
//
// tmux is the one "terminal" that lies about the terminal: it strips the
// outer emulator's env fingerprints, rewrites $TERM, and forwards only
// what it understands. These pin the contract the renderer depends on.
//
// The interesting assertions are environment-dependent, so each case
// states which environment it is asserting about and skips cleanly when
// the harness isn't in it — a test that silently asserts nothing is
// worse than one that says so.
#include "maya/terminal/tmux.hpp"
#include "maya/terminal/ansi.hpp"

#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

namespace {

int fail(const std::string& what) {
    std::println("FAIL: {}", what);
    return 1;
}

// Render escapes visibly for diagnostics.
std::string vis(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == 0x1b) o += "\\e";
        else o += c;
    }
    return o;
}

} // namespace

int main() {
    using namespace maya;

    // ── wrap(): the DCS envelope, ESC-DOUBLED ────────────────────────
    // Doubling is mandatory — tmux un-doubles on the way out, so a
    // single ESC inside the payload would terminate the DCS early and
    // the tail would be interpreted by tmux (or printed as garbage).
    if (tmux::active()) {
        const std::string w = tmux::wrap("\x1b[?2026h");
        if (!w.starts_with("\x1bPtmux;"))
            return fail("wrap() must open with the DCS intro, got " + vis(w));
        if (!w.ends_with("\x1b\\"))
            return fail("wrap() must close with ST, got " + vis(w));
        if (w.find("\x1b\x1b[") == std::string::npos)
            return fail("wrap() must DOUBLE the payload ESC, got " + vis(w));
        std::println("wrap(inside tmux) = {}", vis(w));
    } else {
        // Outside tmux wrapping is the identity, so a call site can wrap
        // unconditionally without branching.
        const std::string_view raw = "\x1b[?2026h";
        if (tmux::wrap(raw) != raw)
            return fail("wrap() must be the identity outside tmux");
        std::println("wrap(outside tmux) = identity \u2713");
    }

    // ── Sync markers resolve to something that ARRIVES ───────────────
    // Outside tmux: the plain DEC private mode.
    // Inside tmux + passthrough: the SAME sequence, DCS-wrapped, because
    //   a RAW ?2026 is swallowed by tmux (measured) — the body would
    //   render un-synced, i.e. tearing with none of the benefit.
    // Inside tmux without passthrough: EMPTY, because neither form can
    //   arrive; emitting half a wrapper is worse than skipping it.
    if (!tmux::active()) {
        if (tmux::sync_begin() != ansi::sync_start
            || tmux::sync_end() != ansi::sync_end)
            return fail("outside tmux, sync markers must be the plain DEC mode");
        if (!tmux::sync_available())
            return fail("outside tmux, sync must be available");
    } else if (tmux::passthrough_allowed()) {
        if (!tmux::sync_available())
            return fail("tmux + passthrough must yield usable sync markers");
        const std::string_view b = tmux::sync_begin();
        if (!b.starts_with("\x1bPtmux;"))
            return fail("tmux + passthrough: sync_begin must be DCS-wrapped, got "
                        + vis(b));
        if (b.find("2026") == std::string_view::npos)
            return fail("tmux + passthrough: sync_begin lost its payload");
        std::println("sync_begin(tmux+passthrough) = {}", vis(b));
    } else {
        if (tmux::sync_available())
            return fail("tmux WITHOUT passthrough cannot sync — markers must "
                        "be empty rather than unreachable bytes");
        std::println("sync unavailable (tmux, passthrough off) \u2713");
    }

    // ── Features are self-consistent ─────────────────────────────────
    // Outside tmux every feature answers false: the query is about what
    // TMUX will forward, and there is no tmux. (Callers fall back to
    // their own env heuristics — see the header's asymmetry note.)
    if (!tmux::active()) {
        for (auto f : {tmux::Feature::Sync, tmux::Feature::CursorStyle,
                       tmux::Feature::Focus, tmux::Feature::Clipboard,
                       tmux::Feature::Rgb})
            if (tmux::has_feature(f))
                return fail("outside tmux no feature may report true");
        std::println("features(outside tmux) = all false \u2713");
    } else {
        // Inside tmux we can't assert WHICH features exist (that depends
        // on the outer terminal), but the query must be stable and must
        // not crash — and a feature we just observed must stay observed.
        const bool a = tmux::has_feature(tmux::Feature::CursorStyle);
        const bool b = tmux::has_feature(tmux::Feature::CursorStyle);
        if (a != b) return fail("has_feature() must be stable across calls");
        std::println("features(tmux): cstyle={} focus={} sync={} clipboard={}",
                     tmux::has_feature(tmux::Feature::CursorStyle),
                     tmux::has_feature(tmux::Feature::Focus),
                     tmux::has_feature(tmux::Feature::Sync),
                     tmux::has_feature(tmux::Feature::Clipboard));
    }

    // ── ansi:: aliases delegate to the module (one implementation) ───
    if (ansi::tmux_in_path() != tmux::active())
        return fail("ansi::tmux_in_path() must agree with tmux::active()");
    if (ansi::wrap_for_tmux("\x1b[0m") != tmux::wrap("\x1b[0m"))
        return fail("ansi::wrap_for_tmux() must agree with tmux::wrap()");

    // ── env_supports_synchronized_output() is tmux-aware ─────────────
    // Inside tmux it must reflect what can actually ARRIVE, not an
    // env-sniff of a terminal whose fingerprints tmux stripped.
    if (tmux::active()
        && ansi::env_supports_synchronized_output() != tmux::sync_available())
        return fail("inside tmux, sync support must equal tmux::sync_available()");

    std::println("PASS: tmux capability layer (wrap / sync / features / aliases)");
    return 0;
}
