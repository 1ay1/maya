#pragma once
// maya - A compile-time type-safe TUI library for C++26
//
// Umbrella header. Include this single file to get the entire maya API.
// Headers are ordered bottom-up: foundational types first, then styling,
// terminal I/O, layout, elements, rendering, and finally the application
// entry point. Each layer depends only on layers above it in this list.

// -- Core: types, error handling, concepts, reactive signals, SIMD ----------
#include <maya/core/types.hpp>
#include <maya/core/expected.hpp>
#include <maya/core/concepts.hpp>
#include <maya/core/signal.hpp>
#include <maya/core/simd.hpp>

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
#include <maya/app/canvas_app.hpp>
#include <maya/app/events.hpp>
#include <maya/app/run.hpp>
