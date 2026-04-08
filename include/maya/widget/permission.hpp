#pragma once
// maya::widget::permission — Tool permission prompt card
//
// Zed's bordered card presentation + Claude Code's key-hint UX.
//
//   ╭─ ⚠ Permission Required ─────────────╮
//   │ bash wants to execute:               │
//   │ rm -rf node_modules && npm install   │
//   │                                      │
//   │ [y] allow  [n] deny  [a] always      │
//   ╰──────────────────────────────────────╯

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../element/builder.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class PermissionResult : uint8_t { Pending, Allow, AllowAlways, Deny };

class Permission {
public:
    struct Config {
        Config() = default;
        std::string tool_name;
        std::string description;
        bool show_always_allow = false;
    };

    explicit Permission(Config cfg) : cfg_(std::move(cfg)) {}

    Permission(std::string tool_name, std::string description) {
        cfg_.tool_name = std::move(tool_name);
        cfg_.description = std::move(description);
    }

    [[nodiscard]] PermissionResult result() const { return result_; }
    void set_result(PermissionResult r) { result_ = r; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        std::vector<Element> rows;

        // Action line: "tool wants to execute:"
        std::string verb = "wants to execute:";
        if (cfg_.tool_name == "read" || cfg_.tool_name == "Read")
            verb = "wants to read:";
        else if (cfg_.tool_name == "edit" || cfg_.tool_name == "Edit"
              || cfg_.tool_name == "write" || cfg_.tool_name == "Write")
            verb = "wants to edit:";

        {
            std::string content = cfg_.tool_name + " " + verb;
            std::vector<StyledRun> runs;
            runs.push_back(StyledRun{0, cfg_.tool_name.size(),
                Style{}.with_bold()});
            runs.push_back(StyledRun{cfg_.tool_name.size(),
                content.size() - cfg_.tool_name.size(), Style{}.with_dim()});
            rows.push_back(Element{TextElement{
                .content = std::move(content), .style = {}, .runs = std::move(runs),
            }});
        }

        // Description (the actual command/path)
        if (!cfg_.description.empty()) {
            rows.push_back(Element{TextElement{
                .content = cfg_.description,
                .style = Style{}.with_fg(Color::rgb(200, 204, 212)),
            }});
        }

        // Spacer
        rows.push_back(Element{TextElement{.content = ""}});

        // Key hints: [y] allow  [n] deny  [a] always
        {
            std::string content;
            std::vector<StyledRun> runs;
            auto dim = Style{}.with_dim();
            auto allow_key = Style{}.with_bold().with_fg(Color::rgb(152, 195, 121));
            auto deny_key = Style{}.with_bold().with_fg(Color::rgb(224, 108, 117));

            auto add_hint = [&](const char* key, const char* label, Style ks) {
                std::size_t off = content.size();
                content += "[";
                runs.push_back(StyledRun{off, 1, dim});
                off = content.size();
                content += key;
                runs.push_back(StyledRun{off, std::string_view{key}.size(), ks});
                off = content.size();
                std::string rest = std::string{"] "} + label + "  ";
                content += rest;
                runs.push_back(StyledRun{off, rest.size(), dim});
            };

            add_hint("y", "allow", allow_key);
            add_hint("n", "deny", deny_key);
            if (cfg_.show_always_allow)
                add_hint("a", "always", allow_key);

            rows.push_back(Element{TextElement{
                .content = std::move(content), .style = {},
                .wrap = TextWrap::NoWrap, .runs = std::move(runs),
            }});
        }

        // Wrap in Zed-style bordered card with warning tint
        return detail::box()
            .border(BorderStyle::Round)
            .border_color(Color::rgb(120, 100, 50))
            .border_text(" \xe2\x9a\xa0 Permission Required ",
                         BorderTextPos::Top, BorderTextAlign::Start)
            .padding(0, 1, 0, 1)(
                detail::vstack()(std::move(rows))
            );
    }

private:
    Config cfg_;
    PermissionResult result_ = PermissionResult::Pending;
};

} // namespace maya
