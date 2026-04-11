#pragma once
// maya::widget::model_badge — Colored model indicator
//
// Displays the active AI model with a color-coded dot and badge.
// Recognizes Claude model families and colors them distinctively.
//
//   ModelBadge badge("claude-opus-4-6");
//   auto ui = badge.build();

#include <string>
#include <string_view>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

class ModelBadge {
    std::string model_;
    bool        compact_ = false;

    struct ModelInfo {
        std::string display_name;
        Color       color;
    };

    [[nodiscard]] ModelInfo resolve() const {
        // Opus: purple
        if (model_.find("opus") != std::string::npos)
            return {"Opus", Color::rgb(198, 120, 221)};
        // Sonnet: blue
        if (model_.find("sonnet") != std::string::npos)
            return {"Sonnet", Color::rgb(97, 175, 239)};
        // Haiku: green
        if (model_.find("haiku") != std::string::npos)
            return {"Haiku", Color::rgb(152, 195, 121)};
        // GPT models: teal
        if (model_.find("gpt") != std::string::npos)
            return {"GPT", Color::rgb(86, 182, 194)};
        // Gemini: amber
        if (model_.find("gemini") != std::string::npos)
            return {"Gemini", Color::rgb(229, 192, 123)};
        // Unknown
        return {model_, Color::rgb(171, 178, 191)};
    }

    // Extract version from model ID (e.g., "4-6" from "claude-opus-4-6")
    [[nodiscard]] std::string extract_version() const {
        // Look for version pattern: digit-digit or digit.digit
        for (size_t i = 0; i + 2 < model_.size(); ++i) {
            char c = model_[i];
            if (c >= '0' && c <= '9') {
                char sep = model_[i + 1];
                if ((sep == '-' || sep == '.') && model_[i + 2] >= '0' && model_[i + 2] <= '9') {
                    size_t end = i + 3;
                    while (end < model_.size() && (model_[end] >= '0' && model_[end] <= '9'))
                        ++end;
                    auto ver = model_.substr(i, end - i);
                    // Normalize dashes to dots for display
                    for (auto& ch : ver) if (ch == '-') ch = '.';
                    return ver;
                }
            }
        }
        return {};
    }

public:
    ModelBadge() = default;
    explicit ModelBadge(std::string model) : model_(std::move(model)) {}

    void set_model(std::string m) { model_ = std::move(m); }
    void set_compact(bool b) { compact_ = b; }

    [[nodiscard]] const std::string& model() const { return model_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        using namespace dsl;

        auto info = resolve();

        if (compact_) {
            return h(
                text("\xe2\x97\x8f ", Style{}.with_fg(info.color)), // ●
                text(info.display_name, Style{}.with_fg(info.color).with_bold())
            ).build();
        }

        auto version = extract_version();
        std::vector<Element> parts;

        parts.push_back(text("\xe2\x97\x8f ", Style{}.with_fg(info.color))); // ●
        parts.push_back(text(info.display_name, Style{}.with_fg(info.color).with_bold()));

        if (!version.empty()) {
            parts.push_back(text(" " + version,
                Style{}.with_fg(info.color).with_dim()));
        }

        return h(std::move(parts)).build();
    }
};

} // namespace maya
