#pragma once
// maya::widget::file_ref — File path reference display
//
// Renders a file path with the directory dimmed, filename underlined like a
// link, and optional line number in purple, matching Zed's file link display.
//
// Usage:
//   FileRef ref("src/render/canvas.cpp", 42);
//   auto ui = ref.build();

#include <string>
#include <string_view>
#include <vector>

#include "../element/builder.hpp"
#include "../style/style.hpp"

namespace maya {

struct FileRef {
    std::string path;
    int line = 0;

    struct Config {
        Config() = default;
        Style path_style   = Style{}.with_dim().with_fg(Color::rgb(150, 156, 170));
        Style name_style   = Style{}.with_fg(Color::rgb(97, 175, 239)).with_underline();
        Style lineno_style = Style{}.with_fg(Color::rgb(198, 160, 246));
        std::string icon   = "\xf0\x9f\x93\x84 "; // 📄 + space
        bool show_icon     = true;
    };

    FileRef(std::string path, int line = 0)
        : path(std::move(path)), line(line) {}

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const { return build(Config{}); }
    [[nodiscard]] Element build(Config cfg) const {
        std::string content;
        std::vector<StyledRun> runs;

        // Icon
        if (cfg.show_icon && !cfg.icon.empty()) {
            std::size_t off = content.size();
            content += cfg.icon;
            runs.push_back({off, cfg.icon.size(), Style{}});
        }

        // Split path into directory + filename
        std::string_view sv = path;
        std::size_t last_sep = sv.rfind('/');
        if (last_sep == std::string_view::npos) {
            last_sep = sv.rfind('\\');
        }

        if (last_sep != std::string_view::npos) {
            // Directory part (including trailing slash)
            std::string_view dir = sv.substr(0, last_sep + 1);
            std::size_t dir_off = content.size();
            content += dir;
            runs.push_back({dir_off, dir.size(), cfg.path_style});

            // Filename part
            std::string_view name = sv.substr(last_sep + 1);
            std::size_t name_off = content.size();
            content += name;
            runs.push_back({name_off, name.size(), cfg.name_style});
        } else {
            // No directory, just filename
            std::size_t name_off = content.size();
            content += path;
            runs.push_back({name_off, path.size(), cfg.name_style});
        }

        // Line number
        if (line > 0) {
            std::string lineno = ":" + std::to_string(line);
            std::size_t ln_off = content.size();
            content += lineno;
            runs.push_back({ln_off, lineno.size(), cfg.lineno_style});
        }

        return Element{TextElement{
            .content = std::move(content),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};
    }
};

} // namespace maya
