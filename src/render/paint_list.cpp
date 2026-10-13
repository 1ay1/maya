// src/render/paint_list.cpp — a list of elements, owned or borrowed.
#include "render_internal.hpp"

namespace maya {
namespace render_detail {

void paint_list(const PaintCtx& c, const ElementList& node) {
    Canvas& canvas = c.canvas;
    StylePool& pool = c.pool;
    const auto& layout_nodes = c.layout_nodes;
    const auto& ln = c.ln;
    const int ax = c.ax;
    const int ay = c.ay;
    // Fragment: recurse into each child with its own layout node.
    for (const auto& [child, child_layout_idx] :
             std::views::zip(node.items, ln.children)) {
        paint_element(
            child,
            canvas,
            pool,
            layout_nodes,
            child_layout_idx,
            ax,
            ay);
    }
}

void paint_list_ref(const PaintCtx& c, const ElementListRef& node) {
    Canvas& canvas = c.canvas;
    StylePool& pool = c.pool;
    const auto& layout_nodes = c.layout_nodes;
    const auto& ln = c.ln;
    const int ax = c.ax;
    const int ay = c.ay;
    const int aw = c.aw;
    // Borrowed-fragment paint: identical shape to ElementList,
    // but reads through the application-supplied pointer. Zero
    // copy of the items vector — only the per-child paint
    // recursion happens here.
    if (!node.items_ref) return;
    // Witness Chain — Trim Accounting write-back. When the
    // fragment is ledger-tagged, stamp each block's laid-out
    // height into the ledger BEFORE painting it. These heights
    // come from the SAME layout::compute pass whose positions
    // the compose serializes this frame, at the live width —
    // they ARE the wire heights, by construction. The ledger
    // mints trim-commit counts exclusively from these stamps,
    // which is what makes a host-side measurement drift
    // (the historical trim-corruption class) unrepresentable.
    const bool ledger_ok = node.ledger != nullptr
        && node.items_ref == &node.ledger->elements()
        && node.items_ref->size() == ln.children.size();
    // Also stamp the width the blocks were laid out at. This is
    // the constraint compute() handed the fragment's children —
    // the SAME width a block sealed next update cycle will be
    // laid out at in the freeze frame. seal_measured() warms
    // the measure cache at this width, which is what retires
    // the host's "reconstruct the content width by subtracting
    // the chrome paddings" fossil (the -4 comment stack).
    if (ledger_ok) node.ledger->record_paint_width(aw);
    std::size_t block_idx = 0;
    for (const auto& [child, child_layout_idx] :
             std::views::zip(*node.items_ref, ln.children)) {
        if (ledger_ok) {
            node.ledger->record_paint(
                block_idx++,
                layout_nodes[child_layout_idx]
                    .computed.size.height.raw());
        }
        paint_element(
            child,
            canvas,
            pool,
            layout_nodes,
            child_layout_idx,
            ax,
            ay);
    }
}

} // namespace render_detail
} // namespace maya
