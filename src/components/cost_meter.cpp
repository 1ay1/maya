#include "maya/components/cost_meter.hpp"

namespace maya::components {

namespace detail {

std::string format_tokens(int n) {
    if (n >= 1'000'000) return fmt("%5.1fM", n / 1'000'000.0);
    if (n >= 1'000)     return fmt("%5.1fk", n / 1'000.0);
    return fmt("%5d", n);
}

} // namespace detail

Element CostMeter(CostMeterProps props) {
    using namespace maya::dsl;
    auto& p = palette();

    auto format_tok = detail::format_tokens;

    // Cost color based on budget ratio
    auto cost_color = [&]() -> Color {
        if (props.budget <= 0) return p.success;
        double ratio = props.cost / props.budget;
        if (ratio > 0.8) return p.error;
        if (ratio > 0.5) return p.warning;
        return p.success;
    };

    auto cost_str = fmt("$%.4f", props.cost);
    Style label_style = Style{}.with_fg(p.muted);
    Style value_style = Style{}.with_fg(p.text);

    // ── Compact mode ────────────────────────────────────────────────────
    if (props.compact) {
        std::vector<Element> parts;

        if (!props.model.empty()) {
            parts.push_back(text(props.model, value_style));
            parts.push_back(text(" │ ", label_style));
        }

        parts.push_back(text(format_tok(props.input_tokens) + " in", label_style));
        parts.push_back(text(" / ", Style{}.with_fg(p.dim)));
        parts.push_back(text(format_tok(props.output_tokens) + " out", label_style));
        parts.push_back(text(" │ ", label_style));
        parts.push_back(text(cost_str, Style{}.with_fg(cost_color())));

        return hstack()(std::move(parts));
    }

    // ── Full mode ───────────────────────────────────────────────────────
    auto row = [&](std::string_view label, Element value) {
        return hstack().gap(1)(
            text(fmt(" %-7s", std::string(label).c_str()), label_style),
            std::move(value)
        );
    };

    std::vector<Element> rows;

    if (!props.model.empty())
        rows.push_back(row("Model", text(props.model, value_style)));

    rows.push_back(row("Input",
        text(format_tok(props.input_tokens) + " tokens", value_style)));

    rows.push_back(row("Output",
        text(format_tok(props.output_tokens) + " tokens", value_style)));

    if (props.cache_read > 0 || props.cache_write > 0) {
        auto cache_str = format_tok(props.cache_read) + " read / " +
                         format_tok(props.cache_write) + " write";
        rows.push_back(row("Cache", text(cache_str, value_style)));
    }

    rows.push_back(row("Cost",
        text(cost_str, Style{}.with_fg(cost_color()))));

    if (props.budget > 0) {
        double ratio = std::clamp(props.cost / props.budget, 0.0, 1.0);
        int w = 20;
        int filled = static_cast<int>(ratio * w);

        std::string bar;
        for (int i = 0; i < filled; ++i) bar += "█";
        for (int i = filled; i < w; ++i) bar += "░";

        auto budget_str = fmt(" $%.2f / $%.2f", props.cost, props.budget);
        Color bar_color = cost_color();

        rows.push_back(row("Budget", hstack()(
            text(bar, Style{}.with_fg(bar_color)),
            text(budget_str, value_style)
        )));
    }

    return vstack()(std::move(rows));
}

} // namespace maya::components
