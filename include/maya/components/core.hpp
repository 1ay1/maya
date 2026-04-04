#pragma once
// maya::components::core — React-like component primitives
//
// Components are functions that take Props and return Element.
// Stateful components use Signal<T> for state (like useState).
// Composition works through children vectors and the DSL.
//
//   auto ui = Callout({.severity = Severity::Warning,
//                      .title = "Heads up",
//                      .children = { text("Check your config") }});

#include <maya/maya.hpp>
#include <maya/dsl.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace maya::components {

// ── Utility: snprintf wrapper ────────────────────────────────────────────────

template <typename... A>
inline std::string fmt(const char* f, A... a) {
    char b[256];
    std::snprintf(b, sizeof b, f, a...);
    return b;
}

// ── Children type ────────────────────────────────────────────────────────────
// Like React's `children` prop — a vector of Elements.

using Children = std::vector<Element>;

// ── Severity enum (shared across Callout, Badge, etc.) ───────────────────────

enum class Severity { Info, Success, Warning, Error };

// ── Status enum (for ToolCard, Spinner, etc.) ────────────────────────────────

enum class TaskStatus {
    Idle, Pending, InProgress, Completed, Failed, Canceled, WaitingForConfirmation
};

// ── Variant enum (for Button, Chip styling) ──────────────────────────────────

enum class Variant { Filled, Outlined, Tinted, Ghost };

// ── Size enum ────────────────────────────────────────────────────────────────

enum class Size { Small, Medium, Large };

// ── Semantic colors ──────────────────────────────────────────────────────────
// Palette that maps semantic meaning to concrete colors.
// Users can override this to theme the entire component library.

struct Palette {
    Color primary   = Color::rgb(100, 160, 255);
    Color secondary = Color::rgb(160, 140, 255);
    Color accent    = Color::rgb(80, 220, 200);

    Color success   = Color::rgb(80, 220, 120);
    Color warning   = Color::rgb(240, 200, 60);
    Color error     = Color::rgb(240, 80, 80);
    Color info      = Color::rgb(100, 180, 255);

    Color text      = Color::rgb(210, 210, 225);
    Color muted     = Color::rgb(100, 100, 120);
    Color dim       = Color::rgb(60, 60, 78);
    Color surface   = Color::rgb(22, 22, 32);
    Color bg        = Color::rgb(14, 14, 22);
    Color border    = Color::rgb(45, 48, 65);

    Color diff_add  = Color::rgb(80, 220, 120);
    Color diff_del  = Color::rgb(240, 80, 80);
    Color diff_mod  = Color::rgb(240, 200, 60);
};

// Thread-local palette — set once at app startup, used by all components.
inline Palette& palette() {
    static thread_local Palette p;
    return p;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

inline Color severity_color(Severity sev) {
    auto& p = palette();
    switch (sev) {
        case Severity::Info:    return p.info;
        case Severity::Success: return p.success;
        case Severity::Warning: return p.warning;
        case Severity::Error:   return p.error;
    }
    return p.info;
}

inline const char* severity_icon(Severity sev) {
    switch (sev) {
        case Severity::Info:    return "ℹ";
        case Severity::Success: return "✓";
        case Severity::Warning: return "⚠";
        case Severity::Error:   return "✗";
    }
    return "•";
}

inline Color status_color(TaskStatus st) {
    auto& p = palette();
    switch (st) {
        case TaskStatus::Idle:       return p.muted;
        case TaskStatus::Pending:    return p.warning;
        case TaskStatus::InProgress: return p.info;
        case TaskStatus::Completed:  return p.success;
        case TaskStatus::Failed:     return p.error;
        case TaskStatus::Canceled:              return p.dim;
        case TaskStatus::WaitingForConfirmation: return p.warning;
    }
    return p.muted;
}

inline const char* status_icon(TaskStatus st) {
    switch (st) {
        case TaskStatus::Idle:                   return "○";
        case TaskStatus::Pending:                return "◔";
        case TaskStatus::InProgress:             return "●";
        case TaskStatus::Completed:              return "✓";
        case TaskStatus::Failed:                 return "✗";
        case TaskStatus::Canceled:               return "⊘";
        case TaskStatus::WaitingForConfirmation: return "⚑";
    }
    return "○";
}

// ── Spinner frames ───────────────────────────────────────────────────────────

namespace spinners {

inline constexpr const char* braille[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
inline constexpr int braille_n = 10;

inline constexpr const char* dots[] = {
    "⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"
};
inline constexpr int dots_n = 8;

inline constexpr const char* line[] = {
    "─", "\\", "│", "/"
};
inline constexpr int line_n = 4;

inline constexpr const char* bounce[] = {
    "⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"
};
inline constexpr int bounce_n = 8;

} // namespace spinners

// ── Spinner tick helper ──────────────────────────────────────────────────────

inline const char* spin(int frame, const char* const* frames = spinners::braille,
                        int n = spinners::braille_n) {
    return frames[frame % n];
}

} // namespace maya::components
