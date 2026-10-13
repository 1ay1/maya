#!/usr/bin/env python3
"""tools/globals_to_model.py FILE --state a,b,c --fns f,g --const-fns h,i [--rng]

One-shot helper for porting a callback-loop example: moves the listed
file-scope `static` state declarations into `struct Model`, gives every
listed function a `Model& m` (or `const Model& m` for --const-fns) first
parameter, qualifies uses of the state names inside function bodies as
`m.name`, and (with --rng) turns the file-static rng + randi/randf into
Model members. Call sites of the listed functions get `m` threaded in.

It does NOT write the program struct or main(): that is per-demo.
"""
import argparse, re

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--state", required=True)
ap.add_argument("--fns", default="")
ap.add_argument("--const-fns", default="")
ap.add_argument("--rng", action="store_true")
a = ap.parse_args()
s = open(a.file).read()
state = [x for x in a.state.split(",") if x]
fns = [x for x in a.fns.split(",") if x]
cfns = [x for x in a.const_fns.split(",") if x]

# 1. pull each state declaration out (single line: `static T name = init;` or `static T name;`)
decls = []
for n in state:
    m = re.search(r"^static (?:inline )?([^\n;=]*?)\b" + n + r"\b(\s*(?:=[^;\n]*|\{[^;\n]*\})?);[^\n]*\n", s, re.M)
    if not m:
        raise SystemExit(f"no declaration for {n}")
    decls.append(f"    {m.group(1).strip()} {n}{m.group(2)};")
    s = s[:m.start()] + s[m.end():]

rng = ""
if a.rng:
    s = re.sub(r"^static std::mt19937 rng[^\n]*\n", "", s, flags=re.M)
    s = re.sub(r"^static int randi\(int lo, int hi\) \{.*?^\}\n", "", s, flags=re.M | re.S)
    s = re.sub(r"^static float randf\(float lo, float hi\) \{.*?^\}\n", "", s, flags=re.M | re.S)
    rng = ("\n    std::mt19937 rng{std::random_device{}()};\n"
           "    int   randi(int lo, int hi)     { return std::uniform_int_distribution<int>(lo, hi)(rng); }\n"
           "    float randf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }\n")

model = "// Everything the screen shows, and the RNG that drives it.\nstruct Model {\n" + "\n".join(decls) + "\n" + rng + "};\n\n"
MODEL_END = "@@MODEL_END@@"
model += MODEL_END

# insert the Model before the first listed function definition
first = min((s.find(re.search(r"^static [^\n]*\b" + f + r"\(", s, re.M).group(0)) for f in fns + cfns
             if re.search(r"^static [^\n]*\b" + f + r"\(", s, re.M)), default=-1)
if first < 0:
    raise SystemExit("no function found to anchor the Model before")
s = s[:first] + model + s[first:]

head, body = s.split(MODEL_END, 1)

# 2. signatures: add the model parameter
for f, q in [(f, "Model& m") for f in fns] + [(f, "const Model& m") for f in cfns]:
    body = re.sub(r"^(static [^\n]*\b" + f + r")\(\)", r"\1(" + q + ")", body, flags=re.M)
    body = re.sub(r"^(static [^\n]*\b" + f + r")\((?!\)|" + re.escape(q) + ")", r"\1(" + q + ", ", body, flags=re.M)
# 3. call sites: thread m in
for f in fns + cfns:
    body = re.sub(r"(?<![\w.:])" + f + r"\(\)", f + "(m)", body)
    body = re.sub(r"(?<![\w.:])" + f + r"\((?!m\)|m,|Model|const Model)", f + "(m, ", body)
# 4. qualify state uses
for n in state:
    body = re.sub(r"(?<![\w.>:])" + n + r"\b(?!\s*\()", "m." + n, body)
if a.rng:
    body = re.sub(r"(?<![\w.>:])(randi|randf)\(", r"m.\1(", body)
    body = re.sub(r"(?<![\w.>:])rng\b", "m.rng", body)
open(a.file, "w").write(head + body)
print("ok:", len(state), "state,", len(fns) + len(cfns), "functions")
