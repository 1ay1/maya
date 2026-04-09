#pragma once
// maya — A compile-time type-safe TUI library for C++26
//
// Public API header. Include this single file to get the entire maya API.
//
// This header exposes ONLY the public interface:
//   - DSL          (v, h, t, text, dyn, when, map, pipes, styles)
//   - App          (run, print, live, quit, RunConfig, Ctx)
//   - Events       (key, ctrl, alt, mouse_clicked, on, ...)
//   - Signals      (Signal, Computed, Effect, Batch)
//   - Widgets      (Input, Scrollable, Markdown, ToolCall, ...)
//
// Internal subsystems (render pipeline, canvas, diff engine, SIMD,
// terminal I/O, layout engine) are NOT included. They are implementation
// details that downstream projects should never depend on.
//
// Usage:
//   #include <maya/maya.hpp>
//   using namespace maya::dsl;
//
//   constexpr auto ui = v(
//       t<"Hello"> | Bold | Fg<100, 180, 255>,
//       h(t<"A">, t<"B"> | Dim) | border_<Round> | pad<1>
//   );
//   maya::print(ui.build());

// ── Core: types, error handling, concepts, reactive signals, focus ───────
#include <maya/core/types.hpp>
#include <maya/core/expected.hpp>
#include <maya/core/concepts.hpp>
#include <maya/core/signal.hpp>
#include <maya/core/overload.hpp>
#include <maya/core/scope_exit.hpp>
#include <maya/core/focus.hpp>
#include <maya/core/render_context.hpp>

// ── Style: colors, text styles, borders, themes ─────────────────────────
#include <maya/style/color.hpp>
#include <maya/style/style.hpp>
#include <maya/style/border.hpp>
#include <maya/style/theme.hpp>

// ── Elements: the element variant, concrete types, builder DSL ──────────
#include <maya/element/text.hpp>
#include <maya/element/box.hpp>
#include <maya/element/element.hpp>
#include <maya/element/builder.hpp>

// ── App: lifecycle, events, run() ───────────────────────────────────────
#include <maya/app/context.hpp>
#include <maya/app/app.hpp>
#include <maya/app/events.hpp>
#include <maya/app/run.hpp>
#include <maya/app/inline.hpp>
#include <maya/app/environment.hpp>
#include <maya/app/error_boundary.hpp>
#include <maya/app/static_region.hpp>

// ── DSL: compile-time UI tree builder ───────────────────────────────────
#include <maya/dsl.hpp>

// ── Widgets ─────────────────────────────────────────────────────────────
// Widgets are NOT included here. Include them individually as needed:
//
//   #include <maya/widget/input.hpp>
//   #include <maya/widget/markdown.hpp>
//   #include <maya/widget/scrollable.hpp>
//   ...
//
// See include/maya/widget/ for the full list.
