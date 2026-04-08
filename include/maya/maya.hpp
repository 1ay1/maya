#pragma once
// maya — A compile-time type-safe TUI library for C++26
//
// Umbrella header. Include this single file to get the entire maya API.
//
// Primary API — compile-time DSL (maya::dsl):
//
//   using namespace maya::dsl;
//   constexpr auto ui = v(
//       t<"Hello"> | Bold | Fg<100, 180, 255>,
//       h(t<"A">, t<"B"> | Dim) | border_<Round> | pad<1>
//   );
//   maya::print(ui.build());
//
// Type-state safety: impossible states are compile errors.
// dyn() provides runtime escape hatches for dynamic content.
//
// Headers are ordered bottom-up: foundational types first, then styling,
// terminal I/O, layout, elements, rendering, and finally the application
// entry point. Each layer depends only on layers above it in this list.

// -- Core: types, error handling, concepts, reactive signals, SIMD, focus ----
#include <maya/core/types.hpp>
#include <maya/core/expected.hpp>
#include <maya/core/concepts.hpp>
#include <maya/core/signal.hpp>
#include <maya/core/simd.hpp>
#include <maya/core/overload.hpp>
#include <maya/core/scope_exit.hpp>
#include <maya/core/focus.hpp>
#include <maya/core/render_context.hpp>

// -- Style: colors, text styles, borders, themes ----------------------------
#include <maya/style/color.hpp>
#include <maya/style/style.hpp>
#include <maya/style/border.hpp>
#include <maya/style/theme.hpp>

// -- Terminal: ANSI sequences, terminal state, input parsing, buffered I/O --
#include <maya/terminal/ansi.hpp>
#include <maya/terminal/terminal.hpp>
#include <maya/terminal/input.hpp>
#include <maya/terminal/writer.hpp>

// -- Layout: flexbox-inspired layout engine ---------------------------------
#include <maya/layout/yoga.hpp>

// -- Elements: the element variant, concrete types, builder DSL -------------
#include <maya/element/text.hpp>
#include <maya/element/box.hpp>
#include <maya/element/element.hpp>
#include <maya/element/builder.hpp>

// -- Render: canvas, diffing, rendering pipeline, frame buffer ---------------
#include <maya/render/canvas.hpp>
#include <maya/render/diff.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
#include <maya/render/pipeline.hpp>
#include <maya/render/frame.hpp>

// -- App: context, application entry point, event helpers, run() ------------
#include <maya/app/context.hpp>
#include <maya/app/app.hpp>
#include <maya/app/events.hpp>
#include <maya/app/run.hpp>
#include <maya/app/inline.hpp>

// -- Widgets: high-level interactive components ------------------------------
#include <maya/widget/input.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/spinner.hpp>
#include <maya/widget/select.hpp>
#include <maya/widget/progress.hpp>
#include <maya/widget/divider.hpp>
#include <maya/widget/badge.hpp>
#include <maya/widget/breadcrumb.hpp>
#include <maya/widget/statusbar.hpp>
#include <maya/widget/confirm.hpp>
#include <maya/widget/table.hpp>
#include <maya/widget/toast.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/accordion.hpp>
#include <maya/widget/file_ref.hpp>
#include <maya/widget/tree_view.hpp>
#include <maya/widget/thinking.hpp>
#include <maya/widget/permission.hpp>

// -- App: static region for scrollback freeze --------------------------------
#include <maya/app/static_region.hpp>

// -- DSL: compile-time UI tree builder ----------------------------------------
#include <maya/dsl.hpp>
