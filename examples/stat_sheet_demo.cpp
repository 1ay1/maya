// stat_sheet_demo.cpp — eyeball the sheet at three widths.
#include <cstdio>
#include <string>
#include <vector>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/stat_sheet.hpp>

using namespace maya;

static void dump(const StatSheet& s, int w) {
    StylePool pool;
    Canvas canvas(w, 30, &pool);
    render_tree(s.build(), canvas, pool, theme::dark, true);
    std::printf("\n--- %d cols ---\n", w);
    for (int y = 0; y < 30; ++y) {
        std::string line;
        for (int x = 0; x < w; ++x) {
            const char32_t ch = canvas.get(x, y).character;
            if (ch == 0) { line += ' '; continue; }
            if (ch < 0x80) { line += static_cast<char>(ch); continue; }
            // re-encode UTF-8
            if (ch < 0x800) {
                line += static_cast<char>(0xC0 | (ch >> 6));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                line += static_cast<char>(0xE0 | (ch >> 12));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty() && y > 20) break;
        std::printf("|%s|\n", line.c_str());
    }
}

int main() {
    StatSheet s;
    s.indent(1);
    s.hero("62%", "of routed turns ran below the Strategic model");
    s.blank();
    s.heading("By role");
    s.entry({.label = "Strategic",      .value = "12", .detail = "38%", .share = 0.38});
    s.entry({.label = "Implementation", .value = "14", .detail = "44%", .share = 0.44});
    s.entry({.label = "Utility",        .value = "6",  .detail = "18%", .share = 0.18});
    s.blank();
    s.heading("By model");
    s.entry({.label = "claude-sonnet-4-6", .value = "12", .detail = "38%", .share = 0.38});
    s.entry({.label = "glm-4.6",           .value = "14", .detail = "44%", .share = 0.44});
    s.entry({.label = "claude-haiku-4-5",  .value = "6",  .detail = "18%", .share = 0.18});
    s.entry({.label = "gpt-5-mini",        .value = "1",  .detail = "1%",  .share = 0.01});
    s.blank();
    s.heading("Throughput");
    s.entry({.label = "Output rate", .value = "1.2k/s",
             .spark = {1, 4, 2, 8, 3, 7, 9, 5, 6, 8, 4, 9}});
    s.entry({.label = "Context",     .value = "62%", .share = 0.62, .wide = true});

    dump(s, 72);
    dump(s, 44);
    dump(s, 26);
    return 0;
}
