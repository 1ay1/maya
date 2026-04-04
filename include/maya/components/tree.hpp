#pragma once
// maya::components::Tree — Recursive hierarchical view with expand/collapse
//
//   TreeNode<std::string> root{.data = "src", .children = {
//       {.data = "main.cpp"}, {.data = "util.cpp"}
//   }};
//   Tree<std::string> tree({.roots = {root}});
//
//   // In event handler:
//   tree.update(ev);  // Up/Down/Left/Right/Enter/Space
//
//   // In render:
//   tree.render([](const std::string& data, int depth, bool expanded,
//                  bool selected, bool has_children) {
//       return text(data, selected ? Style{}.with_bold() : Style{});
//   })

#include "core.hpp"

namespace maya::components {

template <typename T>
struct TreeNode {
    T                      data;
    std::vector<TreeNode>  children = {};
    bool                   expanded = false;
};

template <typename T>
struct TreeProps {
    std::vector<TreeNode<T>> roots           = {};
    bool                     show_connectors = true;
    int                      max_visible     = 20;
    bool                     wrap            = true;
};

template <typename T>
class Tree {
    struct FlatEntry {
        TreeNode<T>*      node;
        int               depth;
        bool              is_last;
        std::vector<bool> ancestors_last;
    };

    int selected_ = 0;
    int scroll_   = 0;
    TreeProps<T> props_;

    // ── Flattening ──────────────────────────────────────────────────────────

    static void flatten_into(std::vector<FlatEntry>& out,
                             std::vector<TreeNode<T>>& nodes,
                             int depth,
                             const std::vector<bool>& ancestors_last) {
        int n = static_cast<int>(nodes.size());
        for (int i = 0; i < n; ++i) {
            bool last = (i == n - 1);
            out.push_back({&nodes[i], depth, last, ancestors_last});
            if (nodes[i].expanded && !nodes[i].children.empty()) {
                auto new_ancestors = ancestors_last;
                new_ancestors.push_back(last);
                flatten_into(out, nodes[i].children, depth + 1, new_ancestors);
            }
        }
    }

    std::vector<FlatEntry> flatten() const {
        std::vector<FlatEntry> flat;
        flatten_into(flat, const_cast<std::vector<TreeNode<T>>&>(props_.roots),
                     0, {});
        return flat;
    }

    // ── Scrolling ───────────────────────────────────────────────────────────

    void clamp(int total) {
        if (total == 0) { selected_ = 0; return; }
        if (selected_ < 0) selected_ = props_.wrap ? total - 1 : 0;
        if (selected_ >= total) selected_ = props_.wrap ? 0 : total - 1;
        ensure_visible();
    }

    void ensure_visible() {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + props_.max_visible)
            scroll_ = selected_ - props_.max_visible + 1;
        if (scroll_ < 0) scroll_ = 0;
    }

    // ── Parent search ───────────────────────────────────────────────────────

    int find_parent(const std::vector<FlatEntry>& flat, int idx) const {
        if (idx <= 0) return -1;
        int target_depth = flat[idx].depth - 1;
        for (int i = idx - 1; i >= 0; --i) {
            if (flat[i].depth == target_depth) return i;
        }
        return -1;
    }

    // ── Connector prefix ────────────────────────────────────────────────────

    std::string connector_prefix(const FlatEntry& entry) const {
        std::string prefix;
        for (int d = 0; d < entry.depth; ++d) {
            prefix += entry.ancestors_last[d] ? "   " : "│  ";
        }
        prefix += entry.is_last ? "└─ " : "├─ ";
        bool has_children = !entry.node->children.empty();
        if (has_children) {
            prefix += entry.node->expanded ? "▾" : "▸";
        } else {
            prefix += " ";
        }
        return prefix;
    }

public:
    explicit Tree(TreeProps<T> props = {})
        : props_(std::move(props)) {}

    // ── State access ────────────────────────────────────────────────────────

    [[nodiscard]] int selected() const { return selected_; }

    [[nodiscard]] const T* selected_item() const {
        auto flat = flatten();
        if (flat.empty() || selected_ < 0 ||
            selected_ >= static_cast<int>(flat.size()))
            return nullptr;
        return &flat[selected_].node->data;
    }

    void set_roots(std::vector<TreeNode<T>> roots) {
        props_.roots = std::move(roots);
        auto flat = flatten();
        clamp(static_cast<int>(flat.size()));
    }

    // ── Event handling ──────────────────────────────────────────────────────

    bool update(const Event& ev) {
        auto flat = flatten();
        int n = static_cast<int>(flat.size());
        if (n == 0) return false;

        // Up / k
        if (key(ev, SpecialKey::Up) || key(ev, 'k')) {
            --selected_; clamp(n); return true;
        }
        // Down / j
        if (key(ev, SpecialKey::Down) || key(ev, 'j')) {
            ++selected_; clamp(n); return true;
        }
        // Left: collapse or go to parent
        if (key(ev, SpecialKey::Left)) {
            auto& entry = flat[selected_];
            if (entry.node->expanded) {
                entry.node->expanded = false;
                // Re-flatten after collapse, clamp selection
                auto new_flat = flatten();
                clamp(static_cast<int>(new_flat.size()));
            } else {
                int parent = find_parent(flat, selected_);
                if (parent >= 0) { selected_ = parent; ensure_visible(); }
            }
            return true;
        }
        // Right: expand or go to first child
        if (key(ev, SpecialKey::Right)) {
            auto& entry = flat[selected_];
            if (!entry.node->children.empty()) {
                if (!entry.node->expanded) {
                    entry.node->expanded = true;
                    ensure_visible();
                } else {
                    // Move to first child (next entry in flat list)
                    if (selected_ + 1 < n) {
                        ++selected_; ensure_visible();
                    }
                }
            }
            return true;
        }
        // Enter / Space: toggle expand
        if (key(ev, SpecialKey::Enter) || key(ev, ' ')) {
            auto& entry = flat[selected_];
            if (!entry.node->children.empty()) {
                entry.node->expanded = !entry.node->expanded;
                auto new_flat = flatten();
                clamp(static_cast<int>(new_flat.size()));
            }
            return true;
        }
        // Page Up / Page Down
        if (key(ev, SpecialKey::PageUp)) {
            selected_ -= props_.max_visible; clamp(n); return true;
        }
        if (key(ev, SpecialKey::PageDown)) {
            selected_ += props_.max_visible; clamp(n); return true;
        }
        // Home / End
        if (key(ev, SpecialKey::Home)) {
            selected_ = 0; clamp(n); return true;
        }
        if (key(ev, SpecialKey::End)) {
            selected_ = n - 1; clamp(n); return true;
        }

        return false;
    }

    // ── Render ──────────────────────────────────────────────────────────────
    // render_item: (const T& data, int depth, bool expanded,
    //               bool selected, bool has_children) -> Element

    template <typename RenderFn>
    [[nodiscard]] Element render(RenderFn&& render_item) const {
        using namespace maya::dsl;

        auto flat = flatten();
        int n = static_cast<int>(flat.size());

        if (n == 0) {
            return text("  (empty)", Style{}.with_fg(palette().dim).with_italic());
        }

        int vis_end = std::min(n, scroll_ + props_.max_visible);
        std::vector<Element> rows;
        rows.reserve(vis_end - scroll_);

        for (int i = scroll_; i < vis_end; ++i) {
            auto& entry = flat[i];
            bool is_selected = (i == selected_);
            bool has_children = !entry.node->children.empty();

            if (props_.show_connectors) {
                std::string prefix = connector_prefix(entry);
                auto item_el = render_item(entry.node->data, entry.depth,
                                           entry.node->expanded, is_selected,
                                           has_children);
                rows.push_back(
                    hstack()(
                        text(prefix, Style{}.with_fg(palette().dim)),
                        std::move(item_el)
                    )
                );
            } else {
                rows.push_back(render_item(entry.node->data, entry.depth,
                                           entry.node->expanded, is_selected,
                                           has_children));
            }
        }

        // Scroll indicators
        if (n > props_.max_visible) {
            std::string indicator;
            if (scroll_ > 0) indicator += "↑ ";
            indicator += std::to_string(selected_ + 1) + "/" + std::to_string(n);
            if (vis_end < n) indicator += " ↓";
            rows.push_back(text(indicator, Style{}.with_fg(palette().dim)));
        }

        return vstack()(std::move(rows));
    }
};

} // namespace maya::components
