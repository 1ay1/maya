#pragma once
// maya::internal — Full header including rendering internals
//
// Use this in code that needs direct access to Canvas, StylePool,
// diff engine, terminal I/O, or the layout engine. This is for
// maya's own examples and advanced use cases like canvas_run().
//
// Downstream projects should use <maya/maya.hpp> instead.

#include <maya/maya.hpp>

// -- Internal subsystems (not part of the public API) ----------------------
#include <maya/core/simd.hpp>
#include <maya/terminal/ansi.hpp>
#include <maya/terminal/terminal.hpp>
#include <maya/terminal/input.hpp>
#include <maya/terminal/writer.hpp>
#include <maya/layout/yoga.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/diff.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
#include <maya/render/pipeline.hpp>
#include <maya/render/frame.hpp>
