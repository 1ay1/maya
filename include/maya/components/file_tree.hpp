#pragma once
// maya::components::FileTree — Stateful file tree with icons and git status
//
//   auto roots = FileTree::from_paths({"src/main.cpp", "src/lib/util.hpp", "README.md"});
//   FileTree tree({.roots = std::move(roots)});
//
//   // In event handler:
//   tree.update(ev);
//
//   // In render:
//   tree.render()

#include "core.hpp"
#include "tree.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace maya::components {

// ── Git status ──────────────────────────────────────────────────────────────

enum class GitStatus { None, Modified, Added, Deleted, Renamed, Untracked };

// ── File entry ──────────────────────────────────────────────────────────────

struct FileEntry {
    std::string name;
    std::string path;
    bool        is_directory = false;
    GitStatus   git_status   = GitStatus::None;
};

// ── Props ───────────────────────────────────────────────────────────────────

struct FileTreeProps {
    std::vector<TreeNode<FileEntry>> roots = {};
    bool show_icons       = true;
    bool show_git_status  = true;
    int  max_visible      = 20;
};

// ── File icon mapping ───────────────────────────────────────────────────────

const char* file_icon(std::string_view name, bool is_dir, bool expanded);

// ── Git status indicator ────────────────────────────────────────────────────

Element git_indicator(GitStatus status);

// ── FileTree component ──────────────────────────────────────────────────────

class FileTree {
    Tree<FileEntry> tree_;
    bool show_icons_;
    bool show_git_status_;

public:
    explicit FileTree(FileTreeProps props)
        : tree_(TreeProps<FileEntry>{
              .roots       = std::move(props.roots),
              .max_visible = props.max_visible,
          })
        , show_icons_(props.show_icons)
        , show_git_status_(props.show_git_status) {}

    // ── Build tree from flat path list ──────────────────────────────────────

    static std::vector<TreeNode<FileEntry>> from_paths(
            const std::vector<std::string>& paths);

    // ── State access ────────────────────────────────────────────────────────

    [[nodiscard]] const FileEntry* selected_file() const {
        return tree_.selected_item();
    }

    [[nodiscard]] std::string selected_path() const;

    // ── Event handling ──────────────────────────────────────────────────────

    bool update(const Event& ev) {
        return tree_.update(ev);
    }

    // ── Render ──────────────────────────────────────────────────────────────

    [[nodiscard]] Element render() const;
};

} // namespace maya::components
