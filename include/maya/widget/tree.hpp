#pragma once
// maya::widget::tree — Hierarchical tree view with expand/collapse
//
// Keyboard-navigable tree structure with indented rendering.
// Supports expand/collapse via Enter/Space/Arrow keys.
//
//   ▾ src
//     ▾ widget
//         list.hpp
//         tree.hpp
//     ▸ core (5 items)
//       main.cpp
//
// Usage:
//   TreeNode root{"src", {
//       {"widget", {{"list.hpp"}, {"tree.hpp"}}},
//       {"core", {{"types.hpp"}, {"signal.hpp"}}},
//       {"main.cpp"},
//   }};
//   Tree tree(std::move(root));
//   tree.on_select([](std::string_view label) { ... });
//   auto ui = tree.build();

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/focus.hpp"
#include "../core/overload.hpp"
#include "../core/signal.hpp"
#include "../dsl.hpp"
#include "../style/style.hpp"
#include "../terminal/input.hpp"

namespace maya {

// ============================================================================
// TreeNode — recursive node in the tree
// ============================================================================

struct TreeNode {
    std::string label;
    std::vector<TreeNode> children;
    bool expanded = false;
    bool selected = false;
};

// ============================================================================
// TreeConfig — appearance configuration
// ============================================================================

struct TreeConfig {
    std::string expanded_icon  = "\xe2\x96\xbe ";   // "▾ "
    std::string collapsed_icon = "\xe2\x96\xb8 ";   // "▸ "
    std::string leaf_prefix    = "  ";
    int indent_width           = 2;
    Style active_style   = Style{}.with_bold().with_fg(Color::blue());
    Style branch_style   = Style{};
    Style leaf_style     = Style{};
    Style count_style    = Style{}.with_dim();
};

// ============================================================================
// Tree — hierarchical tree view widget
// ============================================================================

class Tree {
    TreeNode root_;
    Signal<int> cursor_{0};
    FocusNode focus_;
    TreeConfig cfg_;

    std::move_only_function<void(std::string_view)> on_select_;

    // A flattened row for rendering
    struct FlatRow {
        TreeNode* node;
        int depth;
        int index_in_flat;  // for identification
    };

    // Build flat list of visible nodes
    void flatten(TreeNode& node, int depth, std::vector<FlatRow>& out) const {
        out.push_back(FlatRow{&node, depth, static_cast<int>(out.size())});
        if (!node.children.empty() && node.expanded) {
            for (auto& child : node.children) {
                flatten(child, depth + 1, out);
            }
        }
    }

    [[nodiscard]] std::vector<FlatRow> flatten_all() const {
        std::vector<FlatRow> rows;
        // Use const_cast since we need mutable pointers for toggle,
        // but flatten itself doesn't mutate
        auto& mutable_root = const_cast<TreeNode&>(root_);
        for (auto& child : mutable_root.children) {
            const_cast<Tree*>(this)->flatten(child, 0, rows);
        }
        return rows;
    }

    // Find parent index in flat list
    static int find_parent(const std::vector<FlatRow>& rows, int idx) {
        if (idx <= 0) return -1;
        int target_depth = rows[static_cast<size_t>(idx)].depth;
        for (int i = idx - 1; i >= 0; --i) {
            if (rows[static_cast<size_t>(i)].depth < target_depth) return i;
        }
        return -1;
    }

public:
    explicit Tree(TreeNode root, TreeConfig cfg = {})
        : root_(std::move(root)), cfg_(std::move(cfg)) {}

    // -- Accessors --
    [[nodiscard]] const Signal<int>& cursor()      const { return cursor_; }
    [[nodiscard]] FocusNode& focus_node()                 { return focus_; }
    [[nodiscard]] const FocusNode& focus_node()     const { return focus_; }
    [[nodiscard]] TreeNode& root()                        { return root_; }
    [[nodiscard]] const TreeNode& root()            const { return root_; }

    // -- Callback --
    template <std::invocable<std::string_view> F>
    void on_select(F&& fn) { on_select_ = std::forward<F>(fn); }

    // -- Key event handling --
    [[nodiscard]] bool handle(const KeyEvent& ev) {
        if (!focus_.focused()) return false;

        auto rows = flatten_all();
        if (rows.empty()) return false;

        int cur = cursor_();
        if (cur >= static_cast<int>(rows.size())) {
            cursor_.set(static_cast<int>(rows.size()) - 1);
            cur = cursor_();
        }

        return std::visit(overload{
            [&](SpecialKey sk) -> bool {
                switch (sk) {
                    case SpecialKey::Up:
                        cursor_.set(cur > 0 ? cur - 1 : static_cast<int>(rows.size()) - 1);
                        return true;
                    case SpecialKey::Down:
                        cursor_.set((cur + 1) % static_cast<int>(rows.size()));
                        return true;
                    case SpecialKey::Right: {
                        auto* node = rows[static_cast<size_t>(cur)].node;
                        if (!node->children.empty() && !node->expanded) {
                            node->expanded = true;
                        } else if (!node->children.empty() && node->expanded) {
                            // Move to first child
                            if (cur + 1 < static_cast<int>(rows.size()))
                                cursor_.set(cur + 1);
                        }
                        return true;
                    }
                    case SpecialKey::Left: {
                        auto* node = rows[static_cast<size_t>(cur)].node;
                        if (!node->children.empty() && node->expanded) {
                            node->expanded = false;
                        } else {
                            // Go to parent
                            int parent = find_parent(rows, cur);
                            if (parent >= 0) cursor_.set(parent);
                        }
                        return true;
                    }
                    case SpecialKey::Enter:
                    case SpecialKey::Tab: {
                        auto* node = rows[static_cast<size_t>(cur)].node;
                        if (!node->children.empty()) {
                            node->expanded = !node->expanded;
                        }
                        if (on_select_) on_select_(node->label);
                        return true;
                    }
                    default: return false;
                }
            },
            [&](CharKey ck) -> bool {
                if (ck.codepoint == 'j') {
                    cursor_.set((cur + 1) % static_cast<int>(rows.size()));
                    return true;
                }
                if (ck.codepoint == 'k') {
                    cursor_.set(cur > 0 ? cur - 1 : static_cast<int>(rows.size()) - 1);
                    return true;
                }
                if (ck.codepoint == ' ') {
                    auto* node = rows[static_cast<size_t>(cur)].node;
                    if (!node->children.empty()) {
                        node->expanded = !node->expanded;
                    }
                    return true;
                }
                return false;
            },
        }, ev.key);
    }

private:
    void build_row(const FlatRow& row, int cur, bool focused,
                   std::vector<Element>& output) const {
        const auto* node = row.node;
        bool active = (row.index_in_flat == cur);
        bool is_branch = !node->children.empty();

        std::string line;
        std::vector<StyledRun> runs;

        // Indentation
        int indent = row.depth * cfg_.indent_width;
        if (indent > 0) {
            line.append(static_cast<size_t>(indent), ' ');
        }

        // Icon / prefix
        size_t icon_start = line.size();
        if (is_branch) {
            line += node->expanded ? cfg_.expanded_icon : cfg_.collapsed_icon;
        } else {
            line += cfg_.leaf_prefix;
        }

        // Label
        size_t label_start = line.size();
        line += node->label;

        // Style for the whole line (icon + label)
        Style line_style;
        if (active) {
            line_style = focused ? cfg_.active_style : Style{}.with_bold();
        } else if (is_branch) {
            line_style = cfg_.branch_style;
        } else {
            line_style = cfg_.leaf_style;
        }
        runs.push_back(StyledRun{icon_start, line.size() - icon_start, line_style});

        // Children count for collapsed branches
        if (is_branch && !node->expanded) {
            std::string count_str = " (" + std::to_string(node->children.size()) + " items)";
            size_t count_start = line.size();
            line += count_str;
            runs.push_back(StyledRun{count_start, count_str.size(), cfg_.count_style});
        }

        // Indent run (invisible spacer)
        if (indent > 0) {
            runs.insert(runs.begin(), StyledRun{0, static_cast<size_t>(indent), Style{}});
        }

        output.push_back(Element{TextElement{
            .content = std::move(line),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }});
    }

public:
    // -- Node concept: build into Element --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto rows = flatten_all();
        int cur = cursor_();
        bool focused = focus_.focused();

        if (rows.empty()) {
            return Element{TextElement{
                .content = "(empty)",
                .style = cfg_.count_style,
            }};
        }

        std::vector<Element> output;
        output.reserve(rows.size());

        for (const auto& row : rows) {
            build_row(row, cur, focused, output);
        }

        return dsl::v(std::move(output)).build();
    }
};

} // namespace maya
