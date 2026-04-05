#pragma once
// maya::components::Callout — Info/success/warning/error notification box
//
//   Callout({.severity = Severity::Warning,
//            .title = "Deprecated API",
//            .body = "This endpoint will be removed in v3."})
//
//   Callout({.severity = Severity::Error,
//            .title = "Build failed",
//            .children = { text("Exit code 1"), text("See logs") }})

#include "core.hpp"

namespace maya::components {

struct CalloutProps {
    Severity    severity = Severity::Info;
    std::string title    = "";
    std::string body     = "";
    Children    children = {};
};

Element Callout(CalloutProps props = {});

} // namespace maya::components
