#pragma once
// maya::widget::file_changes — Session file change summary
//
// Shows all files modified during an agent session with status
// indicators (created, modified, deleted) and line change counts.
//
//   FileChanges fc;
//   fc.add("src/auth.ts", FileChangeKind::Modified, 12, 3);
//   fc.add("src/token.ts", FileChangeKind::Created, 45, 0);
//   fc.add("src/old.ts", FileChangeKind::Deleted, 0, 30);
//   auto ui = fc.build();

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

enum class FileChangeKind : uint8_t {
    Created,
    Modified,
    Deleted,
    Renamed,
};

struct FileChange {
    std::string    path;
    FileChangeKind kind        = FileChangeKind::Modified;
    int            lines_added = 0;
    int            lines_removed = 0;
};

class FileChanges {
    std::vector<FileChange> changes_;
    bool compact_ = false;

    static const char* kind_icon(FileChangeKind k) {
        switch (k) {
            case FileChangeKind::Created:  return "+";
            case FileChangeKind::Modified: return "~";
            case FileChangeKind::Deleted:  return "-";
            case FileChangeKind::Renamed:  return "\xe2\x86\x92"; // →
        }
        return "~";
    }

    static Color kind_color(FileChangeKind k) {
        switch (k) {
            case FileChangeKind::Created:  return Color::green();
            case FileChangeKind::Modified: return Color::yellow();
            case FileChangeKind::Deleted:  return Color::red();
            case FileChangeKind::Renamed:  return Color::blue();
        }
        return Color::yellow();
    }

public:
    FileChanges() = default;

    void add(std::string path, FileChangeKind kind = FileChangeKind::Modified,
             int added = 0, int removed = 0) {
        changes_.push_back({std::move(path), kind, added, removed});
    }
    void add(FileChange fc) { changes_.push_back(std::move(fc)); }
    void clear() { changes_.clear(); }
    void set_compact(bool b) { compact_ = b; }

    [[nodiscard]] std::size_t size() const { return changes_.size(); }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto lbl = Style{}.with_dim();
        auto val = Style{};

        if (changes_.empty())
            return text("No files changed", lbl);

        // Summary counts. Only the totals are rendered; the per-kind
        // counters were tracked for a future "by-kind summary" row that
        // never landed. Dropped to silence -Wunused-but-set-variable
        // on clang; restore alongside that feature when it ships.
        int total_add = 0, total_rm = 0;
        for (auto& c : changes_) {
            total_add += c.lines_added;
            total_rm  += c.lines_removed;
        }

        // ── Compact: single summary line ─────────────────────────────
        if (compact_) {
            std::vector<Element> parts;
            parts.push_back(text(std::to_string(changes_.size()) + " files", val));
            if (total_add > 0)
                parts.push_back(text(" +" + std::to_string(total_add),
                    Style{}.with_fg(Color::green())));
            if (total_rm > 0)
                parts.push_back(text(" -" + std::to_string(total_rm),
                    Style{}.with_fg(Color::red())));
            return h(std::move(parts)).build();
        }

        // ── Full: file list with change counts ───────────────────────
        std::vector<Element> rows;

        // Summary header
        {
            std::vector<Element> summary;
            summary.push_back(text(std::to_string(changes_.size()) + " files changed",
                val.with_bold()));
            if (total_add > 0)
                summary.push_back(text("  +" + std::to_string(total_add),
                    Style{}.with_fg(Color::green())));
            if (total_rm > 0)
                summary.push_back(text("  -" + std::to_string(total_rm),
                    Style{}.with_fg(Color::red())));
            rows.push_back(h(std::move(summary)).build());
        }

        // Max path width for alignment
        int max_path = 0;
        for (auto& c : changes_)
            max_path = std::max(max_path, static_cast<int>(c.path.size()));
        max_path = std::min(max_path, 50);

        // File list
        for (auto& c : changes_) {
            Color kc = kind_color(c.kind);
            std::vector<Element> line;

            // Kind indicator
            line.push_back(text("  "));
            line.push_back(text(kind_icon(c.kind), Style{}.with_fg(kc).with_bold()));
            line.push_back(text(" "));

            // File path
            line.push_back(text(c.path, val));

            // Line change counts
            if (c.lines_added > 0 || c.lines_removed > 0) {
                int pad = max_path - static_cast<int>(c.path.size());
                if (pad < 2) pad = 2;
                line.push_back(text(std::string(static_cast<size_t>(pad), ' ')));

                if (c.lines_added > 0)
                    line.push_back(text("+" + std::to_string(c.lines_added),
                        Style{}.with_fg(Color::green())));
                if (c.lines_added > 0 && c.lines_removed > 0)
                    line.push_back(text(" "));
                if (c.lines_removed > 0)
                    line.push_back(text("-" + std::to_string(c.lines_removed),
                        Style{}.with_fg(Color::red())));
            }

            rows.push_back(h(std::move(line)).build());
        }

        return v(std::move(rows)).build();
    }
};

} // namespace maya
