#!/usr/bin/env python3
"""Split component headers: extract inline bodies into .cpp files.

For each .hpp, creates a matching .cpp with function/method bodies,
leaving only declarations in the .hpp. Handles:
- inline free functions
- class method bodies (non-trivial, multi-line)
- Default argument removal in .cpp definitions
- Nested braces, strings, comments
"""

import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HPP_DIR = ROOT / "include" / "maya" / "components"
CPP_DIR = ROOT / "src" / "components"

# Must stay header-only (templates or meta)
SKIP = {"core.hpp", "components.hpp",
        "list.hpp", "select.hpp", "tree.hpp", "radio_group.hpp", "scroll_view.hpp"}


def match_brace(text, pos):
    """Return index of matching '}' for '{' at pos."""
    assert text[pos] == '{', f"Expected '{{' at pos {pos}, got '{text[pos]}'"
    depth = 0
    i = pos
    n = len(text)
    while i < n:
        c = text[i]
        if c in ('"', "'"):
            q = c
            i += 1
            while i < n and text[i] != q:
                if text[i] == '\\': i += 1
                i += 1
            i += 1
            continue
        if c == '/' and i+1 < n and text[i+1] == '/':
            while i < n and text[i] != '\n': i += 1
            continue
        if c == '/' and i+1 < n and text[i+1] == '*':
            i += 2
            while i < n and not (text[i] == '*' and i+1 < n and text[i+1] == '/'):
                i += 1
            i += 2
            continue
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return i
        i += 1
    return -1


def find_classes(text):
    """Find class definitions, return list of (name, body_start_brace, body_end_brace)."""
    results = []
    for m in re.finditer(r'\bclass\s+(\w+)\b[^{;]*\{', text):
        name = m.group(1)
        brace = m.end() - 1
        end = match_brace(text, brace)
        if end > 0:
            results.append((name, brace, end))
    return results


def process(hpp_path):
    text = hpp_path.read_text()
    name = hpp_path.stem

    ns_match = re.search(r'namespace\s+([\w:]+)\s*\{', text)
    if not ns_match:
        return None
    namespace = ns_match.group(1)

    classes = find_classes(text)

    def in_class(pos):
        for (cn, s, e) in classes:
            if s < pos < e:
                return cn
        return None

    # Collect all replacements: (start, end, new_hpp_text, cpp_definition)
    replacements = []

    # === FREE FUNCTIONS: inline ... { body } ===
    for m in re.finditer(r'^([ \t]*)inline\s+', text, re.MULTILINE):
        start = m.start()
        if in_class(start):
            continue  # skip inline methods inside classes

        # Find opening brace after signature
        rest = text[m.end():]
        paren_depth = 0
        found_paren = False
        brace_rel = -1
        for i, c in enumerate(rest):
            if c == '(': paren_depth += 1; found_paren = True
            elif c == ')': paren_depth -= 1
            elif c == '{' and paren_depth == 0 and found_paren:
                brace_rel = i; break
            elif c == ';' and paren_depth == 0: break
        if brace_rel < 0:
            continue

        brace_abs = m.end() + brace_rel
        body_end = match_brace(text, brace_abs)
        if body_end < 0:
            continue

        # Check if template (skip)
        before = text[max(0, start-200):start]
        if re.search(r'template\s*<[^>]*>\s*$', before):
            continue

        sig_text = text[m.end():brace_abs].strip()
        body = text[brace_abs:body_end + 1]
        full_end = body_end + 1

        # Skip small one-liners
        body_inner = body[1:-1].strip()
        if '\n' not in body_inner and len(body_inner) < 100:
            continue

        indent = m.group(1)
        # hpp: declaration (remove inline, replace body with ;)
        hpp_decl = f"{indent}{sig_text};\n"
        # cpp: definition (remove default args from signature)
        cpp_sig = re.sub(r'\s*=\s*[^,)]+', '', sig_text)
        cpp_def = f"{cpp_sig} {body}\n"

        replacements.append((start, full_end, hpp_decl, cpp_def))

    # === CLASS METHODS (non-trivial bodies) ===
    for (cls_name, cls_brace, cls_end) in classes:
        # Scan inside the class body for method definitions
        # We work on the full text but only match within [cls_brace+1, cls_end)
        region = text[cls_brace+1:cls_end]
        offset = cls_brace + 1

        for m in re.finditer(
            r'^([ \t]+)'                              # indent
            r'((?:\[\[nodiscard\]\]\s*)?'             # optional [[nodiscard]]
            r'(?:static\s+)?'                         # optional static
            r'[\w:*&<>, ]+?\s+'                       # return type (non-greedy)
            r'(\w+)\s*\([^)]*\))'                     # method name + params
            r'((?:\s*const)?)'                        # trailing const
            r'((?:\s*noexcept)?)'                     # trailing noexcept
            r'\s*\{',                                  # opening brace
            region, re.MULTILINE
        ):
            method_name = m.group(3)
            full_sig = m.group(2)
            is_const = m.group(4).strip()
            is_noexcept = m.group(5).strip()
            indent = m.group(1)

            # Skip constructors/destructors
            if method_name == cls_name or method_name.startswith('~'):
                continue

            # Find the opening brace position in the full text
            match_end_in_region = m.end()
            brace_pos = offset + match_end_in_region - 1
            if text[brace_pos] != '{':
                # Search forward for it
                brace_pos = text.find('{', offset + m.start())
                if brace_pos < 0 or brace_pos >= cls_end:
                    continue

            body_end = match_brace(text, brace_pos)
            if body_end < 0 or body_end > cls_end:
                continue

            body = text[brace_pos:body_end + 1]
            body_inner = body[1:-1].strip()

            # Skip small one-liners
            if '\n' not in body_inner and len(body_inner) < 100:
                continue

            # Skip template methods
            before_in_region = region[max(0, m.start()-100):m.start()]
            last_line = before_in_region.split('\n')[-1] if before_in_region else ''
            if 'template' in last_line:
                continue

            abs_start = offset + m.start()
            abs_end = body_end + 1

            # hpp: keep everything up to and including ')' + const + noexcept, replace body with ;
            qualifiers = ''
            if is_const: qualifiers += ' const'
            if is_noexcept: qualifiers += ' noexcept'
            hpp_decl = f"{indent}{full_sig}{qualifiers};\n"

            # cpp: ClassName::method
            cpp_sig = re.sub(r'\[\[nodiscard\]\]\s*', '', full_sig)
            cpp_sig = re.sub(r'\bstatic\s+', '', cpp_sig)
            cpp_sig = re.sub(r'\s*=\s*[^,)]+', '', cpp_sig)
            paren = cpp_sig.find('(')
            if paren >= 0:
                before = cpp_sig[:paren].rstrip()
                after = cpp_sig[paren:]
                parts = before.rsplit(None, 1)
                if len(parts) == 2:
                    cpp_sig = f"{parts[0]} {cls_name}::{parts[1]}{after}"
                else:
                    cpp_sig = f"{cls_name}::{before}{after}"
            cpp_def = f"{cpp_sig}{qualifiers} {body}\n"

            replacements.append((abs_start, abs_end, hpp_decl, cpp_def))

    if not replacements:
        return None

    # Sort by position descending
    replacements.sort(key=lambda r: r[0], reverse=True)

    new_hpp = text
    cpp_defs = []

    for (start, end, hpp_decl, cpp_def) in replacements:
        new_hpp = new_hpp[:start] + hpp_decl + new_hpp[end:]
        cpp_defs.append(cpp_def)

    cpp_defs.reverse()

    cpp_text = f'#include "maya/components/{name}.hpp"\n\n'
    cpp_text += f'namespace {namespace} {{\n\n'
    cpp_text += '\n'.join(cpp_defs)
    cpp_text += f'\n}} // namespace {namespace}\n'

    return new_hpp, cpp_text


def main():
    CPP_DIR.mkdir(parents=True, exist_ok=True)

    ok = skip = 0
    for hpp in sorted(HPP_DIR.glob("*.hpp")):
        if hpp.name in SKIP:
            skip += 1; continue

        result = process(hpp)
        if result is None:
            print(f"  NOOP {hpp.name}")
            skip += 1; continue

        new_hpp, cpp_text = result

        # Sanity: new_hpp should still end with proper closing
        if '}' in new_hpp[-100:]: pass  # good
        else:
            print(f"  WARN {hpp.name}: hpp doesn't end with }}, skipping")
            skip += 1; continue

        hpp.write_text(new_hpp)
        (CPP_DIR / f"{hpp.stem}.cpp").write_text(cpp_text)
        print(f"  OK   {hpp.name}")
        ok += 1

    print(f"\n{ok} split, {skip} skipped")


if __name__ == "__main__":
    main()
