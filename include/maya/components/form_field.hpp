#pragma once
// maya::components::FormField — Label + description + validation wrapper for inputs
//
//   FormField({.label = "Username", .required = true,
//              .error = "Username is already taken",
//              .children = { text_input.render() }})

#include "core.hpp"

namespace maya::components {

struct FormFieldProps {
    std::string label       = "";
    std::string description = "";
    std::string error       = "";
    bool        required    = false;
    Children    children    = {};
};

Element FormField(FormFieldProps props = {});

} // namespace maya::components
