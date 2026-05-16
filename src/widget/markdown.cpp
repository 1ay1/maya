// markdown.cpp — Facade. The original ~5200-line monolithic
// implementation was carved into:
//
//   widget/markdown/parser.cpp     lexer + inline + block parsers + ref defs
//   widget/markdown/syntax.cpp     language-aware syntax highlighter
//   widget/markdown/render.cpp     AST → Element (colors, blocks, tables)
//   widget/markdown/streaming.cpp  markdown() LRU + StreamingMarkdown
//
// Every public symbol declared in <maya/widget/markdown.hpp> is now defined
// across the four TUs above. This stub stays so any out-of-tree CMakeLists
// that hard-coded the path keeps resolving until it migrates to the
// per-component layout.
