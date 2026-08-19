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
    std::string fallback_label_;   // host-supplied pretty name for unknown ids
    bool        compact_ = false;

    struct ModelInfo {
        std::string display_name;
        Color       color;
    };

    [[nodiscard]] ModelInfo resolve() const {
        // Opus: magenta
        if (model_.find("opus") != std::string::npos)
            return {"Opus", Color::magenta()};
        // Sonnet: blue
        if (model_.find("sonnet") != std::string::npos)
            return {"Sonnet", Color::blue()};
        // Haiku: green
        if (model_.find("haiku") != std::string::npos)
            return {"Haiku", Color::green()};
        // GPT models: cyan
        if (model_.find("gpt") != std::string::npos)
            return {"GPT", Color::cyan()};
        // Gemini: yellow
        if (model_.find("gemini") != std::string::npos)
            return {"Gemini", Color::yellow()};
        // Unknown Claude FAMILY (a model line newer than this table): show
        // the title-cased family word, never the raw id — "claude-fable-…"
        // reads "Fable", not a wall of id bytes in the status bar.
        if (model_.rfind("claude-", 0) == 0) {
            std::string fam;
            for (std::size_t i = 7; i < model_.size(); ++i) {
                const char c = model_[i];
                const bool alpha =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (!alpha) break;
                fam.push_back(fam.empty() && c >= 'a' && c <= 'z'
                                  ? static_cast<char>(c - 'a' + 'A') : c);
            }
            if (!fam.empty()) return {fam, Color::magenta()};
        }
        // Unknown: prefer the host's pretty label over the raw id.
        return {fallback_label_.empty() ? model_ : fallback_label_,
                Color::white()};
    }

    // Extract version from model ID (e.g., "4-6" from "claude-opus-4-6").
    // Both digit runs must be SHORT (≤2 digits) — an 8-digit date stamp like
    // "20260115" is provenance, not a version, and must not match.
    [[nodiscard]] std::string extract_version() const {
        for (size_t i = 0; i + 2 < model_.size(); ++i) {
            char c = model_[i];
            if (c >= '0' && c <= '9') {
                // Length of this digit run.
                size_t run = i;
                while (run < model_.size() && model_[run] >= '0' && model_[run] <= '9')
                    ++run;
                if (run - i > 2) { i = run; continue; }   // date-like: skip run
                if (run >= model_.size()) break;
                const char sep = model_[run];
                if ((sep == '-' || sep == '.') && run + 1 < model_.size()
                    && model_[run + 1] >= '0' && model_[run + 1] <= '9') {
                    size_t end = run + 1;
                    while (end < model_.size() && (model_[end] >= '0' && model_[end] <= '9'))
                        ++end;
                    if (end - (run + 1) > 2) { i = run; continue; }  // x.20260115
                    auto ver = model_.substr(i, end - i);
                    for (auto& ch : ver) if (ch == '-') ch = '.';
                    return ver;
                }
                i = run;   // no separator+digit after the run: keep scanning
            }
        }
        return {};
    }

public:
    ModelBadge() = default;
    explicit ModelBadge(std::string model) : model_(std::move(model)) {}

    void set_model(std::string m) { model_ = std::move(m); }
    void set_compact(bool b) { compact_ = b; }
    // Pretty name to show when the id's family isn't recognized. The host
    // usually has a richer id→label normalizer than this widget; wiring it
    // here means an unknown model can NEVER render as a raw wire id.
    void set_fallback_label(std::string l) { fallback_label_ = std::move(l); }

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
