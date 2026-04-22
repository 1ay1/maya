#pragma once
// maya::widget::fetch_tool — Web fetch result card
//
// Zed's bordered preview card + Claude Code's URL fetch display.
//
//   ╭─ ✓ Fetch ────────────────────────────╮
//   │ https://api.example.com/v1     1.2s  │
//   │ 200 OK  •  application/json          │
//   │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │
//   │ {"data": [...]}                      │
//   ╰──────────────────────────────────────╯

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dsl.hpp"
#include "../style/border.hpp"
#include "../style/style.hpp"

namespace maya {

enum class FetchStatus : uint8_t { Pending, Fetching, Done, Failed };

class FetchTool {
    std::string url_;
    std::string body_;
    std::string content_type_;
    int status_code_ = 0;
    FetchStatus status_ = FetchStatus::Pending;
    float elapsed_ = 0.0f;
    bool expanded_ = false;
    int max_body_lines_ = 20;

public:
    FetchTool() = default;
    explicit FetchTool(std::string url) : url_(std::move(url)) {}

    void set_url(std::string_view u) { url_ = std::string{u}; }
    void set_body(std::string_view b) { body_ = std::string{b}; }
    void set_content_type(std::string_view ct) { content_type_ = std::string{ct}; }
    void set_status_code(int code) { status_code_ = code; }
    void set_status(FetchStatus s) { status_ = s; }
    void set_elapsed(float seconds) { elapsed_ = seconds; }
    void set_expanded(bool e) { expanded_ = e; }
    void toggle() { expanded_ = !expanded_; }
    void set_max_body_lines(int n) { max_body_lines_ = n; }

    [[nodiscard]] FetchStatus status() const { return status_; }
    [[nodiscard]] bool is_expanded() const { return expanded_; }

    operator Element() const { return build(); }

    [[nodiscard]] Element build() const {
        auto [icon, icon_color] = status_icon();
        std::string border_label = " " + icon + " Fetch ";

        auto border_color = Color::bright_black();
        auto border_style = BorderStyle::Round;
        if (status_ == FetchStatus::Failed) {
            border_color = Color::red();
            border_style = BorderStyle::Dashed;
        }

        std::vector<Element> rows;

        // URL + elapsed
        {
            std::string content = url_;
            std::vector<StyledRun> runs;
            auto url_style = Style{}.with_fg(Color::blue()).with_underline();
            runs.push_back(StyledRun{0, url_.size(), url_style});

            if (elapsed_ > 0.0f) {
                std::string ts = "  " + format_elapsed();
                runs.push_back(StyledRun{content.size(), 2, Style{}});
                runs.push_back(StyledRun{content.size() + 2, ts.size() - 2, Style{}.with_dim()});
                content += ts;
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Status code + content type line
        if (status_code_ > 0 || !content_type_.empty()) {
            std::string content;
            std::vector<StyledRun> runs;

            if (status_code_ > 0) {
                std::string code_str = std::to_string(status_code_);
                std::string reason = http_reason(status_code_);
                std::string full = code_str + " " + reason;

                Style code_style;
                if (status_code_ >= 200 && status_code_ < 300)
                    code_style = Style{}.with_fg(Color::green()).with_bold();
                else if (status_code_ >= 300 && status_code_ < 400)
                    code_style = Style{}.with_fg(Color::yellow()).with_bold();
                else
                    code_style = Style{}.with_fg(Color::red()).with_bold();

                runs.push_back(StyledRun{content.size(), full.size(), code_style});
                content += full;
            }

            if (!content_type_.empty()) {
                if (!content.empty()) {
                    std::string sep = "  \xe2\x80\xa2  ";  // " • "
                    runs.push_back(StyledRun{content.size(), sep.size(), Style{}.with_dim()});
                    content += sep;
                }
                runs.push_back(StyledRun{content.size(), content_type_.size(), Style{}.with_dim()});
                content += content_type_;
            }

            rows.push_back(Element{TextElement{
                .content = std::move(content),
                .style = {},
                .wrap = TextWrap::NoWrap,
                .runs = std::move(runs),
            }});
        }

        // Expanded: show body
        if (expanded_ && !body_.empty()) {
            // Separator
            rows.push_back(Element{ComponentElement{
                .render = [](int w, int /*h*/) -> Element {
                    std::string line;
                    for (int i = 0; i < w; ++i) line += "\xe2\x94\x88";  // ┈
                    return Element{TextElement{
                        .content = std::move(line),
                        .style = Style{}.with_dim(),
                    }};
                },
                .layout = {},
            }});

            auto body_style = Style{};
            std::string_view sv = body_;
            int shown = 0;

            while (!sv.empty()) {
                auto nl = sv.find('\n');
                auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);

                if (max_body_lines_ > 0 && shown >= max_body_lines_) {
                    rows.push_back(Element{TextElement{
                        .content = "...",
                        .style = Style{}.with_dim(),
                    }});
                    break;
                }

                rows.push_back(Element{TextElement{
                    .content = std::string{line},
                    .style = body_style,
                    .wrap = TextWrap::Wrap,
                }});
                ++shown;
            }
        }

        return (dsl::v(std::move(rows))
            | dsl::border(border_style)
            | dsl::bcolor(border_color)
            | dsl::btext(border_label, BorderTextPos::Top, BorderTextAlign::Start)
            | dsl::padding(0, 1, 0, 1)).build();
    }

private:
    struct IconInfo { std::string icon; Color color; };

    [[nodiscard]] IconInfo status_icon() const {
        switch (status_) {
            case FetchStatus::Pending:
                return {"\xe2\x97\x8b", Color::bright_black()};   // ○
            case FetchStatus::Fetching:
                return {"\xe2\x97\x8f", Color::yellow()};         // ●
            case FetchStatus::Done:
                return {"\xe2\x9c\x93", Color::green()};          // ✓
            case FetchStatus::Failed:
                return {"\xe2\x9c\x97", Color::red()};            // ✗
        }
        return {"\xe2\x97\x8b", Color::bright_black()};
    }

    [[nodiscard]] static std::string http_reason(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default: return "";
        }
    }

    [[nodiscard]] std::string format_elapsed() const {
        char buf[32];
        if (elapsed_ < 1.0f) {
            std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(elapsed_ * 1000.0f));
            return buf;
        }
        if (elapsed_ < 60.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(elapsed_));
            return buf;
        }
        int mins = static_cast<int>(elapsed_) / 60;
        float secs = elapsed_ - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(secs));
        return buf;
    }
};

} // namespace maya
