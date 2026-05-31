// markdown.cpp — Facade. The original ~5200-line monolithic implementation
// is split into three modules under widget/markdown/, each a directory of
// small single-responsibility TUs:
//
//   parser/     markdown source → md::Document AST
//     parser.cpp          inline + block parsers, ref-def collector
//     text_transform.cpp  Text post-pass: entities / emoji / URLs / mentions
//     tables.cpp          GFM table line classification + cell splitting
//     html_tag.cpp        one-tag HTML scanner (shared inline + block)
//   render/     md::Document AST → maya::Element
//     render.cpp          blocks / inlines / tables → Element
//     syntax.cpp          language-aware code-block syntax highlighter
//   streaming/  incremental / live rendering
//     streaming.cpp       markdown() LRU + StreamingMarkdown widget
//
// Shared declarations live in <maya/widget/markdown/internal.hpp>; the AST in
// <maya/widget/markdown/ast.hpp>; char-class tables in spec_chars.hpp. Every
// public symbol in <maya/widget/markdown.hpp> is defined across those TUs.
// This file stays as a stable anchor for the component layout.
