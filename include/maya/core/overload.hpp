#pragma once
// maya::core::overload - Overload set helper for std::visit
//
// Combines multiple lambdas into a single callable for exhaustive
// variant visitation. C++26 may standardize this as std::overload;
// until then, we consolidate the pattern here.
//
// Usage:
//   std::visit(overload{
//       [](int x)    { ... },
//       [](float x)  { ... },
//       [](auto x)   { ... },
//   }, some_variant);

namespace maya {

template <typename... Fs>
struct overload : Fs... { using Fs::operator()...; };

} // namespace maya
