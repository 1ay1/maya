#pragma once
// maya/jaal/interop.hpp — maya's value types, as jaal sees them.
//
// jaal's Sendable walks a type's fields to prove a Msg is safe to hand to
// another thread. Two of maya's types can't be walked, so they opt in here —
// once, in the seam, rather than in each program that trips over them.
//
// Nothing else in maya may specialise a jaal trait. If a new maya type needs
// to cross the thread boundary, it is declared HERE, with the reason.

#include <jaal/jaal.hpp>

#include "../core/types.hpp"              // Strong<Tag, T>
#include "../render/scrollback_ledger.hpp"  // ScrollbackDebt

// Strong<Tag, T> (Columns, Rows — inside every MouseEvent and Size) has
// user-declared constructors, so it isn't an aggregate and jaal can't look
// inside; it rejects it. Declared CONDITIONALLY: a Strong<Tag, T> is exactly
// as safe as the T it wraps. (A blanket "true" would also bless
// Strong<Tag, std::string_view>, a borrowed view — the one thing Sendable
// exists to stop.)
template <class Tag, class T>
inline constexpr bool jaal::sendable_opt_in<maya::Strong<Tag, T>> = jaal::Sendable<T>;
template <class Tag, class T>
inline constexpr bool jaal::frozen_opt_in<maya::Strong<Tag, T>> = jaal::Frozen<T>;

// ScrollbackDebt is one int behind a private constructor (only the ledger
// mints one), so jaal can't look inside — but there's nothing inside to
// share. It rides in a Cmd (commit_scrollback), which must be Sendable.
template <> inline constexpr bool jaal::sendable_opt_in<maya::ScrollbackDebt> = true;
template <> inline constexpr bool jaal::frozen_opt_in<maya::ScrollbackDebt>   = true;
