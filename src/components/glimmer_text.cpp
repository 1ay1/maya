#include "maya/components/glimmer_text.hpp"

namespace maya::components {

namespace detail {

Color lerp_color(Color a, Color b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ar = a.r(), ag = a.g(), ab = a.b();
    auto br = b.r(), bg = b.g(), bb = b.b();
    return Color::rgb(
        static_cast<uint8_t>(ar + (br - ar) * t),
        static_cast<uint8_t>(ag + (bg - ag) * t),
        static_cast<uint8_t>(ab + (bb - ab) * t)
    );
}

Color shimmer_color(float t, Color start, Color mid, Color end) {
    if (t < 0.5f) return lerp_color(start, mid, t * 2.f);
    return lerp_color(mid, end, (t - 0.5f) * 2.f);
}

} // namespace detail

Element GlimmerText(GlimmerTextProps props) {
    using namespace maya::dsl;

    if (!props.is_streaming) {
        return text(props.text_content, Style{}.with_fg(props.base_color));
    }

    // Render each character with a shimmer wave
    std::vector<Element> chars;
    int len = static_cast<int>(props.text_content.size());
    float wave_pos = static_cast<float>(props.frame) * 0.15f;

    for (int i = 0; i < len; i++) {
        float dist = static_cast<float>(i) - wave_pos;
        float t = 0.f;
        if (dist >= 0.f && dist < static_cast<float>(props.wave_width)) {
            t = 1.f - (dist / static_cast<float>(props.wave_width));
        }
        Color c = (t > 0.01f)
            ? detail::shimmer_color(t, props.shimmer_start, props.shimmer_mid, props.shimmer_end)
            : props.base_color;
        chars.push_back(text(std::string(1, props.text_content[i]),
                              Style{}.with_fg(c).with_bold()));
    }

    return hstack()(std::move(chars));
}

} // namespace maya::components
