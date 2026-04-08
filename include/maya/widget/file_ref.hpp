#pragma once
// maya::widget::file_ref — Styled file path with optional line number
//
// Renders a clickable-style file reference like "src/main.cpp:42"
// with path dimmed and filename highlighted.
//
// Usage:
//   FileRef ref("src/render/canvas.cpp", 42);
//   FileRef ref("README.md");

#include <string>
#include <string_view>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct FileRefConfig {
    Style path_style     = Style{}.with_dim();
    Style name_style     = Style{}.with_fg(Color::rgb(100, 180, 255)).with_underline();
    Style lineno_style   = Style{}.with_fg(Color::rgb(180, 140, 255));
    Style icon_style     = Style{}.with_dim();
    std::string icon     = "\xf0\x9f\x93\x84 ";  // "📄 "
    bool show_icon       = false;
};

class FileRef {
    std::string path_;
    int line_ = 0;
    FileRefConfig cfg_;

public:
    FileRef() = default;

    explicit FileRef(std::string_view path, int line = 0, FileRefConfig cfg = {})
        : path_(path), line_(line), cfg_(std::move(cfg)) {}

    void set_path(std::string_view p, int line = 0) {
        path_ = std::string{p};
        line_ = line;
    }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> parts;

        if (cfg_.show_icon) {
            parts.push_back(Element{TextElement{
                .content = cfg_.icon, .style = cfg_.icon_style}});
        }

        // Split path into directory + filename
        auto slash = path_.rfind('/');
        if (slash != std::string::npos) {
            parts.push_back(Element{TextElement{
                .content = path_.substr(0, slash + 1),
                .style = cfg_.path_style}});
            parts.push_back(Element{TextElement{
                .content = path_.substr(slash + 1),
                .style = cfg_.name_style}});
        } else {
            parts.push_back(Element{TextElement{
                .content = path_, .style = cfg_.name_style}});
        }

        if (line_ > 0) {
            parts.push_back(Element{TextElement{
                .content = ":" + std::to_string(line_),
                .style = cfg_.lineno_style}});
        }

        return detail::hstack()(std::move(parts));
    }
};

} // namespace maya
