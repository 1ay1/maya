#include "maya/components/heatmap.hpp"

namespace maya::components {

Element Heatmap(HeatmapProps props) {
    using namespace maya::dsl;

    if (props.data.empty()) return text("");

    auto is_default_color = [](const Color& c) {
        return c.kind() == Color::Kind::Named && c.r() == 7 && c.g() == 0 && c.b() == 0;
    };

    Color lo = is_default_color(props.low_color)  ? palette().dim     : props.low_color;
    Color hi = is_default_color(props.high_color)  ? palette().success : props.high_color;

    // Convert colors to RGB for interpolation. Named/Indexed colors use
    // a rough ANSI-to-RGB table for the 8 basic colors; Rgb passes through.
    struct RGB { uint8_t r, g, b; };

    static constexpr RGB ansi_table[] = {
        {0,0,0}, {205,0,0}, {0,205,0}, {205,205,0},
        {0,0,238}, {205,0,205}, {0,205,205}, {229,229,229},
        {127,127,127}, {255,0,0}, {0,255,0}, {255,255,0},
        {92,92,255}, {255,0,255}, {0,255,255}, {255,255,255},
    };

    auto to_rgb = [&](const Color& c) -> RGB {
        if (c.kind() == Color::Kind::Rgb) return {c.r(), c.g(), c.b()};
        if (c.kind() == Color::Kind::Named && c.r() < 16)
            return ansi_table[c.r()];
        // Indexed — fallback to the raw index as grayscale
        uint8_t v = c.r();
        return {v, v, v};
    };

    RGB lo_rgb = to_rgb(lo);
    RGB hi_rgb = to_rgb(hi);

    auto lerp_color = [&](float t) -> Color {
        t = std::clamp(t, 0.f, 1.f);
        auto mix = [t](uint8_t a, uint8_t b) -> uint8_t {
            return static_cast<uint8_t>(
                static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f
            );
        };
        return Color::rgb(mix(lo_rgb.r, hi_rgb.r),
                          mix(lo_rgb.g, hi_rgb.g),
                          mix(lo_rgb.b, hi_rgb.b));
    };

    // Determine max label width for right-alignment
    int max_label = 0;
    for (auto& lbl : props.row_labels) {
        int len = static_cast<int>(lbl.size());
        if (len > max_label) max_label = len;
    }

    // Determine number of columns
    std::size_t num_cols = 0;
    for (auto& row : props.data)
        if (row.size() > num_cols) num_cols = row.size();

    std::vector<Element> rows;

    // Column labels row
    if (!props.col_labels.empty()) {
        std::vector<Element> label_parts;
        // Spacer for row label column
        if (max_label > 0) {
            std::string spacer(static_cast<std::size_t>(max_label + 1), ' ');
            label_parts.push_back(text(std::move(spacer)));
        }
        for (std::size_t c = 0; c < num_cols; ++c) {
            std::string lbl = c < props.col_labels.size() ? props.col_labels[c] : "";
            // Center the label in a 2-char wide column
            if (lbl.size() < 2) {
                std::size_t pad = 2 - lbl.size();
                std::size_t left = pad / 2;
                lbl.insert(0, left, ' ');
                lbl.append(pad - left, ' ');
            } else if (lbl.size() > 2) {
                lbl = lbl.substr(0, 2);
            }
            label_parts.push_back(text(std::move(lbl), Style{}.with_fg(palette().muted)));
        }
        rows.push_back(hstack()(std::move(label_parts)));
    }

    // Data rows
    for (std::size_t r = 0; r < props.data.size(); ++r) {
        std::vector<Element> parts;

        // Row label
        if (max_label > 0) {
            std::string lbl;
            if (r < props.row_labels.size())
                lbl = props.row_labels[r];
            // Right-align
            while (static_cast<int>(lbl.size()) < max_label)
                lbl.insert(lbl.begin(), ' ');
            lbl += ' ';
            parts.push_back(text(std::move(lbl), Style{}.with_fg(palette().text)));
        }

        // Cells
        for (std::size_t c = 0; c < num_cols; ++c) {
            float val = c < props.data[r].size() ? std::clamp(props.data[r][c], 0.f, 1.f) : 0.f;
            Color cell_color = lerp_color(val);

            if (props.show_values) {
                int digit = static_cast<int>(val * 9.f + 0.5f);
                digit = std::clamp(digit, 0, 9);
                std::string cell(1, static_cast<char>('0' + digit));
                cell += ' ';
                parts.push_back(text(std::move(cell), Style{}.with_fg(cell_color)));
            } else {
                parts.push_back(text("\u2588\u2588", Style{}.with_fg(cell_color)));
            }
        }

        rows.push_back(hstack()(std::move(parts)));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
