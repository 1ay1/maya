#pragma once
// maya::widget::popup — Floating tooltip/popup with severity styling
//
// A non-interactive bordered text box tinted by severity level.
//
// Usage:
//   Popup tip;
//   tip.show("File saved successfully", PopupStyle::Info);
//   auto ui = tip.build();

#include <string>
#include <string_view>
#include <cstdint>

#include "../dsl.hpp"
#include "../style/style.hpp"

namespace maya {

// ============================================================================
// PopupStyle — severity-based visual tinting
// ============================================================================

enum class PopupStyle : uint8_t {
    Info,
    Warning,
    Error,
};

// ============================================================================
// Popup — non-interactive styled message box
// ============================================================================

class Popup {
    std::string content_;
    bool visible_ = false;
    PopupStyle style_ = PopupStyle::Info;

public:
    Popup() = default;

    explicit Popup(std::string content, PopupStyle style = PopupStyle::Info)
        : content_(std::move(content)), visible_(true), style_(style) {}

    // -- Visibility --
    void show(std::string_view msg, PopupStyle style = PopupStyle::Info) {
        content_ = std::string{msg};
        style_ = style;
        visible_ = true;
    }
    void hide() { visible_ = false; }
    [[nodiscard]] bool visible() const { return visible_; }

    // -- Build --
    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        if (!visible_) {
            return Element{TextElement{.content = ""}};
        }

        Color border_color;
        Color text_color;
        std::string icon;

        switch (style_) {
            case PopupStyle::Info:
                border_color = Color::rgb(97, 175, 239);
                text_color   = Color::rgb(171, 178, 191);
                icon = "\xe2\x84\xb9 ";  // ℹ
                break;
            case PopupStyle::Warning:
                border_color = Color::rgb(229, 192, 123);
                text_color   = Color::rgb(229, 192, 123);
                icon = "\xe2\x9a\xa0 ";  // ⚠
                break;
            case PopupStyle::Error:
                border_color = Color::rgb(224, 108, 117);
                text_color   = Color::rgb(224, 108, 117);
                icon = "\xe2\x9c\x98 ";  // ✘
                break;
        }

        std::string full = icon + content_;

        std::vector<StyledRun> runs;
        runs.push_back(StyledRun{
            .byte_offset = 0,
            .byte_length = icon.size(),
            .style = Style{}.with_fg(border_color),
        });
        runs.push_back(StyledRun{
            .byte_offset = icon.size(),
            .byte_length = content_.size(),
            .style = Style{}.with_fg(text_color),
        });

        auto text_elem = Element{TextElement{
            .content = std::move(full),
            .style = {},
            .wrap = TextWrap::NoWrap,
            .runs = std::move(runs),
        }};

        return (dsl::v(std::move(text_elem))
            | dsl::border(BorderStyle::Round)
            | dsl::bcolor(border_color)
            | dsl::padding(0, 1)).build();
    }
};

} // namespace maya
