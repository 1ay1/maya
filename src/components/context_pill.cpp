#include "maya/components/context_pill.hpp"

namespace maya::components {

namespace detail {

const char* context_icon(ContextKind kind) {
    switch (kind) {
        case ContextKind::File:        return "📄";
        case ContextKind::Directory:   return "📁";
        case ContextKind::Symbol:      return "🔗";
        case ContextKind::Url:         return "🌐";
        case ContextKind::Selection:   return "✂️";
        case ContextKind::GitDiff:     return "±";
        case ContextKind::Diagnostics: return "⚠";
        case ContextKind::Image:       return "🖼";
        case ContextKind::Thread:      return "💬";
        case ContextKind::Rules:       return "📋";
    }
    return "@";
}

Color context_color(ContextKind kind) {
    auto& p = palette();
    switch (kind) {
        case ContextKind::File:        return p.info;
        case ContextKind::Directory:   return p.accent;
        case ContextKind::Symbol:      return p.primary;
        case ContextKind::Url:         return p.secondary;
        case ContextKind::Selection:   return p.warning;
        case ContextKind::GitDiff:     return Color::rgb(200, 120, 255);
        case ContextKind::Diagnostics: return p.warning;
        case ContextKind::Image:       return p.success;
        case ContextKind::Thread:      return p.muted;
        case ContextKind::Rules:       return p.dim;
    }
    return p.muted;
}

} // namespace detail

Element ContextPill(ContextPillProps props) {
    using namespace maya::dsl;

    Color col = (props.color.r() == 0 && props.color.g() == 0 && props.color.b() == 0)
        ? detail::context_color(props.kind)
        : props.color;

    std::string icon_str = detail::context_icon(props.kind);

    return hstack()(
        text(icon_str + " ", Style{}.with_fg(col)),
        text(props.label, Style{}.with_fg(col).with_bold())
    );
}

Element ContextPillRow(ContextPillRowProps props) {
    using namespace maya::dsl;

    std::vector<Element> pills;
    for (auto& pill_props : props.pills) {
        pills.push_back(ContextPill(pill_props));
    }

    return hstack().gap(2)(std::move(pills));
}

} // namespace maya::components
