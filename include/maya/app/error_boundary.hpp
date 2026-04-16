#pragma once
// maya::ErrorBoundary — Catch and display render errors gracefully
//
// Wraps a render function and catches exceptions, displaying an error
// message instead of crashing the application.
//
// Usage:
//   ErrorBoundary boundary;
//   auto elem = boundary.render([&] { return my_widget.build(); });

#include "../element/builder.hpp"
#include "../element/element.hpp"
#include "../style/style.hpp"
#include "../style/color.hpp"
#include "../style/border.hpp"

#include <exception>
#include <functional>
#include <string>

namespace maya {

class ErrorBoundary {
    std::string last_error_;
    bool has_error_ = false;

public:
    /// Try to render, catching any exceptions.
    [[nodiscard]] Element render(std::function<Element()> fn) noexcept {
        if (has_error_) {
            return render_error();
        }
        try {
            return fn();
        } catch (const std::exception& e) {
            last_error_ = e.what();
            has_error_ = true;
            return render_error();
        } catch (...) {
            last_error_ = "Unknown error";
            has_error_ = true;
            return render_error();
        }
    }

    /// Reset the error state (retry rendering).
    void reset() noexcept {
        has_error_ = false;
        last_error_.clear();
    }

    [[nodiscard]] bool has_error() const noexcept { return has_error_; }
    [[nodiscard]] const std::string& error() const noexcept { return last_error_; }

private:
    [[nodiscard]] Element render_error() const {
        return detail::vstack()
            .border(BorderStyle::Round)
            .border_color(Color::red())
            .padding(0, 1, 0, 1)(
                Element{TextElement{
                    .content = "\xe2\x9a\xa0 Render Error",  // ⚠ Render Error
                    .style   = Style{}.with_bold().with_fg(Color::red()),
                }},
                Element{TextElement{
                    .content = last_error_,
                    .style   = Style{}.with_dim(),
                }}
            );
    }
};

} // namespace maya
