#!/usr/bin/env python3
"""tools/port_canvas.py NAME... : generate examples/jaal_NAME.cpp from NAME.cpp.

A canvas_run demo moves to jaal by changing its entry call and nothing
else, so the port is GENERATED, never hand-edited: the demo's code stays in
one place and the port can't drift from it. Re-run after changing a demo.

    (void)canvas_run(cfg, on_resize, on_event, on_paint);
 -> return run_canvas(cfg, on_resize, on_event, on_paint);
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = """// examples/jaal_{name}.cpp — GENERATED from {name}.cpp by tools/port_canvas.py.
// Do not edit: change {name}.cpp and re-run the tool.
//
// The same demo on jaal: its canvas_run() call becomes run_canvas()
// (maya/jaal/canvas.hpp), which draws it as a `paint` element through
// maya::Screen, so it gets the Screen's flow control (never more than one
// frame ahead of the terminal: `q` is instant over a slow ssh link).
//
// Built only with -DMAYA_WITH_JAAL=ON.
#include <maya/jaal/canvas.hpp>
"""

def port(name):
    src_path = os.path.join(ROOT, "examples", f"{name}.cpp")
    src = open(src_path).read()
    n = len(re.findall(r"\(void\)\s*canvas_run\s*\(", src))
    if n != 1:
        raise SystemExit(f"{name}: expected exactly one `(void)canvas_run(` call, found {n}")
    out = re.sub(r"\(void\)\s*canvas_run\s*\(", "return run_canvas(", src)
    dst = os.path.join(ROOT, "examples", f"jaal_{name}.cpp")
    open(dst, "w").write(HEADER.format(name=name) + out)
    print(f"  {name}.cpp -> jaal_{name}.cpp")

if __name__ == "__main__":
    for name in sys.argv[1:]:
        port(name)
