#pragma once
// maya::env — Runtime environment detection
//
// Detects CI environments, non-TTY pipes, and terminal capabilities.
// Apps should use this to gracefully degrade (e.g., skip ANSI in pipes).

#include <cstdlib>
#include <string_view>

#include "../platform/io.hpp"

namespace maya::env {

/// True if stdout is connected to a terminal (not piped/redirected).
[[nodiscard]] inline bool is_tty() noexcept {
    return platform::is_tty(platform::stdout_handle());
}

/// True if running in a CI environment (GitHub Actions, GitLab CI, etc.).
[[nodiscard]] inline bool is_ci() noexcept {
    // Check common CI environment variables
    static const bool ci = [] {
        for (auto var : {"CI", "GITHUB_ACTIONS", "GITLAB_CI", "CIRCLECI",
                         "TRAVIS", "JENKINS_URL", "BUILDKITE", "TF_BUILD"}) {
            if (std::getenv(var)) return true;
        }
        return false;
    }();
    return ci;
}

/// True if the environment supports interactive rendering.
/// False in CI or when stdout is not a TTY.
[[nodiscard]] inline bool is_interactive() noexcept {
    return is_tty() && !is_ci();
}

/// Color support level.
enum class ColorLevel : uint8_t {
    None = 0,    ///< No color support (dumb terminal, NO_COLOR set)
    Basic = 1,   ///< 16 ANSI colors
    Ansi256 = 2, ///< 256 colors
    TrueColor = 3, ///< 24-bit RGB
};

/// Detect the terminal's color support level.
[[nodiscard]] inline ColorLevel color_level() noexcept {
    // NO_COLOR convention (https://no-color.org/)
    if (std::getenv("NO_COLOR")) return ColorLevel::None;
    if (!is_tty()) return ColorLevel::None;

    // COLORTERM=truecolor or 24bit
    if (auto ct = std::getenv("COLORTERM")) {
        std::string_view s{ct};
        if (s == "truecolor" || s == "24bit") return ColorLevel::TrueColor;
    }

    // TERM containing 256color
    if (auto term = std::getenv("TERM")) {
        std::string_view s{term};
        if (s.find("256color") != std::string_view::npos) return ColorLevel::Ansi256;
    }

    // Default: basic colors if TTY
    return ColorLevel::Basic;
}

} // namespace maya::env
