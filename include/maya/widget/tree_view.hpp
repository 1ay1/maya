#pragma once
// maya::widget::tree_view — File/directory tree display
//
// Renders an indented tree structure with box-drawing connectors.
// Ideal for displaying file trees, project structure, or nested data.
//
// Usage:
//   TreeView tree;
//   tree.add("src/", {
//       tree.leaf("main.cpp"),
//       tree.leaf("app.cpp"),
//       tree.dir("render/", {
//           tree.leaf("canvas.cpp"),
//           tree.leaf("diff.cpp"),
//       }),
//   });

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct TreeViewConfig {
    Style dir_style      = Style{}.with_bold().with_fg(Color::rgb(100, 180, 255));
    Style file_style     = Style{};
    Style connector_style = Style{}.with_fg(Color::rgb(60, 60, 80));
    std::string dir_icon  = "\xf0\x9f\x93\x81 ";  // "📁 "
    std::string file_icon = "  ";
    bool show_icons       = false;
};

struct TreeNode {
    std::string name;
    bool is_dir = false;
    Style custom_style{};
    std::vector<TreeNode> children;
};

class TreeView {
    std::vector<TreeNode> roots_;
    TreeViewConfig cfg_;

public:
    TreeView() = default;
    explicit TreeView(TreeViewConfig cfg) : cfg_(std::move(cfg)) {}

    // -- Building the tree ---------------------------------------------------

    /// Create a leaf node (file).
    [[nodiscard]] static TreeNode leaf(std::string_view name, Style s = {}) {
        return {std::string{name}, false, s, {}};
    }

    /// Create a directory node with children.
    [[nodiscard]] static TreeNode dir(std::string_view name,
                                       std::vector<TreeNode> children) {
        return {std::string{name}, true, {}, std::move(children)};
    }

    /// Add a root node.
    void add(TreeNode node) { roots_.push_back(std::move(node)); }

    /// Add a root directory with children.
    void add(std::string_view name, std::vector<TreeNode> children) {
        roots_.push_back({std::string{name}, true, {}, std::move(children)});
    }

    void clear() { roots_.clear(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;
        for (size_t i = 0; i < roots_.size(); ++i) {
            render_node(rows, roots_[i], "", i == roots_.size() - 1);
        }
        return detail::vstack()(std::move(rows));
    }

private:
    void render_node(std::vector<Element>& rows, const TreeNode& node,
                     const std::string& prefix, bool is_last) const {
        // Connector: "├── " or "└── "
        std::string connector = prefix.empty()
            ? "" : (is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "    // "└── "
                            : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "); // "├── "

        // Icon
        std::string icon;
        if (cfg_.show_icons) {
            icon = node.is_dir ? cfg_.dir_icon : cfg_.file_icon;
        }

        // Style
        Style name_style = (node.custom_style.fg.has_value() || node.custom_style.bold)
            ? node.custom_style
            : (node.is_dir ? cfg_.dir_style : cfg_.file_style);

        std::vector<Element> line_parts;
        if (!prefix.empty()) {
            line_parts.push_back(Element{TextElement{
                .content = prefix, .style = cfg_.connector_style}});
        }
        if (!connector.empty()) {
            // Just the connector chars without the prefix
            std::string conn_chars = is_last
                ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
                : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
            line_parts.push_back(Element{TextElement{
                .content = std::move(conn_chars), .style = cfg_.connector_style}});
        }
        if (!icon.empty()) {
            line_parts.push_back(Element{TextElement{
                .content = std::move(icon), .style = name_style}});
        }
        line_parts.push_back(Element{TextElement{
            .content = node.name, .style = name_style}});

        rows.push_back(detail::hstack()(std::move(line_parts)));

        // Recurse into children
        std::string child_prefix = prefix;
        if (!prefix.empty() || !connector.empty()) {
            child_prefix += is_last
                ? "    "
                : "\xe2\x94\x82   ";  // "│   "
        }
        for (size_t i = 0; i < node.children.size(); ++i) {
            render_node(rows, node.children[i], child_prefix,
                       i == node.children.size() - 1);
        }
    }
};

} // namespace maya
