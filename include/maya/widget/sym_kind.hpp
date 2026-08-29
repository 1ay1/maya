#pragma once
// maya::SymKind — shared symbol-kind enum with codicon glyphs + colours.
//
// Used by Breadcrumb, SymbolOutline, and anything else that shows LSP-style
// document symbols. Colours follow the familiar VSCode symbol palette.

#include <cstdint>

#include "../style/color.hpp"

namespace maya {

enum class SymKind : uint8_t {
    Folder, File, Module, Namespace, Class, Struct, Enum, Interface,
    Function, Method, Property, Field, Variable, Constant,
};

[[nodiscard]] inline const char* sym_glyph(SymKind k) {
    switch (k) {
        case SymKind::Folder:    return "\xef\x81\xbb"; //  folder
        case SymKind::File:      return "\xef\x85\x9b"; //  file
        case SymKind::Module:    return "\xef\x84\xa1"; //  cube
        case SymKind::Namespace: return "\xef\x83\x97"; //  braces
        case SymKind::Class:     return "\xef\x83\xa8"; //  class
        case SymKind::Struct:    return "\xef\x83\x89"; //  struct
        case SymKind::Enum:      return "\xef\x84\x85"; //  enum
        case SymKind::Interface: return "\xef\x83\xa8"; //  interface
        case SymKind::Function:  return "\xc6\x92";      // ƒ function
        case SymKind::Method:    return "\xef\x82\x99"; //  method
        case SymKind::Property:  return "\xef\x82\xad"; //  property
        case SymKind::Field:     return "\xef\x84\x93"; //  field
        case SymKind::Variable:  return "\xef\x84\xa1"; //  variable
        case SymKind::Constant:  return "\xef\x84\xa3"; //  constant
    }
    return "\xef\x85\x9b";
}

[[nodiscard]] inline Color sym_color(SymKind k) {
    switch (k) {
        case SymKind::Folder:
        case SymKind::File:      return Color::hex(0x89B4FA); // blue
        case SymKind::Module:
        case SymKind::Namespace: return Color::hex(0xF9E2AF); // yellow
        case SymKind::Class:
        case SymKind::Struct:
        case SymKind::Interface: return Color::hex(0xFAB387); // orange
        case SymKind::Enum:      return Color::hex(0xF9E2AF);
        case SymKind::Function:
        case SymKind::Method:    return Color::hex(0xCBA6F7); // mauve
        case SymKind::Property:
        case SymKind::Field:     return Color::hex(0x94E2D5); // teal
        case SymKind::Variable:
        case SymKind::Constant:  return Color::hex(0x89DCEB); // sky
    }
    return Color::hex(0x9399B2);
}

} // namespace maya
