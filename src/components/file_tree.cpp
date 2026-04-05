#include "maya/components/file_tree.hpp"

namespace maya::components {

const char* file_icon(std::string_view name, bool is_dir, bool expanded) {
    if (is_dir) return expanded ? "📂" : "📁";
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return "📄";
    auto ext = name.substr(dot + 1);
    if (ext == "cpp" || ext == "cc" || ext == "cxx") return "⚡";
    if (ext == "hpp" || ext == "h" || ext == "hxx")  return "📋";
    if (ext == "py")  return "🐍";
    if (ext == "js" || ext == "ts" || ext == "jsx" || ext == "tsx") return "📜";
    if (ext == "rs")  return "🦀";
    if (ext == "go")  return "🔷";
    if (ext == "md")  return "📝";
    if (ext == "json" || ext == "yaml" || ext == "yml" || ext == "toml") return "⚙";
    if (ext == "sh" || ext == "bash" || ext == "zsh") return "🔧";
    if (ext == "txt" || ext == "log") return "📄";
    if (ext == "lock") return "🔒";
    return "📄";
}

Element git_indicator(GitStatus status) {
    using namespace maya::dsl;
    auto& p = palette();
    switch (status) {
        case GitStatus::Modified:  return text(" M", Style{}.with_fg(p.warning));
        case GitStatus::Added:     return text(" A", Style{}.with_fg(p.success));
        case GitStatus::Deleted:   return text(" D", Style{}.with_fg(p.error));
        case GitStatus::Renamed:   return text(" R", Style{}.with_fg(p.info));
        case GitStatus::Untracked: return text(" ?", Style{}.with_fg(p.dim));
        default: return text("");
    }
}

std::vector<TreeNode<FileEntry>> FileTree::from_paths(
        const std::vector<std::string>& paths) {
    // Map: directory path -> list of children entries
    // We build an intermediate structure then convert to TreeNode<FileEntry>.
    struct IntermediateNode {
        FileEntry entry;
        std::map<std::string, IntermediateNode> children;
    };

    IntermediateNode root;
    root.entry.name = "";
    root.entry.is_directory = true;

    for (const auto& path : paths) {
        // Split by '/'
        std::vector<std::string> parts;
        std::string::size_type start = 0;
        while (start < path.size()) {
            auto pos = path.find('/', start);
            if (pos == std::string::npos) {
                parts.push_back(path.substr(start));
                break;
            }
            parts.push_back(path.substr(start, pos - start));
            start = pos + 1;
        }

        if (parts.empty()) continue;

        // Walk/create intermediate nodes
        IntermediateNode* current = &root;
        std::string accumulated;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!accumulated.empty()) accumulated += '/';
            accumulated += parts[i];

            bool is_leaf = (i == parts.size() - 1);
            auto& child = current->children[parts[i]];
            child.entry.name = parts[i];
            child.entry.path = accumulated;
            if (!is_leaf) {
                child.entry.is_directory = true;
            }
            current = &child;
        }
    }

    // Convert intermediate tree to TreeNode<FileEntry> recursively.
    // Sort: directories first (alphabetical), then files (alphabetical).
    std::function<std::vector<TreeNode<FileEntry>>(const IntermediateNode&)>
        convert = [&](const IntermediateNode& node)
            -> std::vector<TreeNode<FileEntry>> {
        std::vector<TreeNode<FileEntry>> result;
        result.reserve(node.children.size());

        // Collect into dirs and files
        std::vector<const IntermediateNode*> dirs;
        std::vector<const IntermediateNode*> files;

        for (const auto& [name, child] : node.children) {
            if (child.entry.is_directory)
                dirs.push_back(&child);
            else
                files.push_back(&child);
        }

        auto by_name = [](const IntermediateNode* a,
                          const IntermediateNode* b) {
            return a->entry.name < b->entry.name;
        };
        std::sort(dirs.begin(), dirs.end(), by_name);
        std::sort(files.begin(), files.end(), by_name);

        for (const auto* dir : dirs) {
            result.push_back(TreeNode<FileEntry>{
                .data     = dir->entry,
                .children = convert(*dir),
                .expanded = false,
            });
        }
        for (const auto* file : files) {
            result.push_back(TreeNode<FileEntry>{
                .data     = file->entry,
                .children = {},
                .expanded = false,
            });
        }

        return result;
    };

    return convert(root);
}

std::string FileTree::selected_path() const {
    auto* entry = selected_file();
    return entry ? entry->path : std::string{};
}

Element FileTree::render() const {
    using namespace maya::dsl;

    bool icons = show_icons_;
    bool git   = show_git_status_;

    return tree_.render(
        [icons, git](const FileEntry& entry, int /*depth*/,
                     bool expanded, bool selected,
                     bool has_children) -> Element {
            std::vector<Element> parts;

            // Icon
            if (icons) {
                const char* icon = file_icon(
                    entry.name, entry.is_directory,
                    has_children && expanded);
                parts.push_back(text(std::string(icon) + " "));
            }

            // Name
            Style name_style;
            auto& p = palette();
            if (selected) {
                name_style = Style{}.with_bold().with_fg(p.primary);
            } else if (entry.is_directory) {
                name_style = Style{}.with_bold().with_fg(p.text);
            } else {
                name_style = Style{}.with_fg(p.text);
            }
            parts.push_back(text(entry.name, name_style));

            // Git status
            if (git && entry.git_status != GitStatus::None) {
                parts.push_back(git_indicator(entry.git_status));
            }

            return hstack()(std::move(parts));
        });
}

} // namespace maya::components
