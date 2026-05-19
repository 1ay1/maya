#pragma once
// maya::CanvasWitness — Layer 0 of the Witness Chain (Canvas-cache integrity).
//
// The chain documented in docs/internals/witness-chain.md proves T1/T2/T3:
// cursor stays inside the viewport, the shadow buffer matches the wire, and
// scrollback is monotone. Those proofs all *assume* the Canvas's derived
// caches — `last_col_[y]`, `max_y_`, `damage_` — are exact functions of the
// cell buffer. If a widget paints a shorter line over a longer one on the
// same row without first clearing, `last_col_` stays stale-high (writes are
// monotone-up; the only path that lowers it is clear_row/clear/clear_below).
// `diff()` then trusts the stale value, skips trailing-blank emission, and
// the old tail ghost-survives on the wire.
//
// T0 (canvas-cache integrity).
//   For every Canvas value reaching diff(), last_content_col(y) equals the
//   highest x in row y with packed != Cell{}.pack(), and max_content_row()
//   equals the highest y with any such cell, and damage() covers every
//   coordinate where this frame's cells_ differs from the prior frame's.
//
// Proof obligation: there must be no producer of CanvasWitness other than
// verify_canvas(); diff() must consume CanvasWitness&& (never raw Canvas).
//
// Failure modes:
//   - cache_drift  → verify_canvas returns nullopt. Caller's only recovery
//                    is clear_row()+repaint (slow path) or hard-reset.
//   - in-window mutation → diff() re-hashes the moved-in canvas and aborts
//                    on mismatch. Single-threaded renderer; the only way
//                    the hash can change between verify and diff is memory
//                    corruption. Matches the A4 hedge from Layer 3.
//
// Cost: one O(W·H) re-derivation per frame in verify_canvas. Same order as
// the existing verify_shadow scan; both fold into one pass if you stash the
// hashes incrementally on set()/fill() — left as a follow-up.

#include <cstdint>
#include <optional>

#include "canvas.hpp"

namespace maya {

// FNV-1a 64-bit of an arbitrary byte range. Header-inline so the witness
// header is self-contained (no link dep on render libs).
namespace detail {
constexpr std::uint64_t fnv1a64(const void* data, std::size_t n,
                                std::uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}
} // namespace detail

class CanvasWitness {
public:
    // Move-only. Copying would let two parallel views diverge from the
    // canvas the second hash-check is run against.
    CanvasWitness(CanvasWitness&&) noexcept = default;
    CanvasWitness& operator=(CanvasWitness&&) noexcept = default;
    CanvasWitness(const CanvasWitness&) = delete;
    CanvasWitness& operator=(const CanvasWitness&) = delete;

    [[nodiscard]] const Canvas& canvas() const noexcept { return *canvas_; }
    [[nodiscard]] std::uint64_t cells_hash_at_issue() const noexcept { return cells_hash_; }
    [[nodiscard]] std::uint64_t caches_hash_at_issue() const noexcept { return caches_hash_; }

private:
    CanvasWitness(const Canvas* c, std::uint64_t cells_h, std::uint64_t caches_h) noexcept
        : canvas_(c), cells_hash_(cells_h), caches_hash_(caches_h) {}

    friend std::optional<CanvasWitness> verify_canvas(const Canvas&) noexcept;

    const Canvas* canvas_;
    std::uint64_t cells_hash_;
    std::uint64_t caches_hash_;
};

// Sole producer. Re-derives last_col_[y] and max_y_ from cells_ the slow way
// (O(W·H)), compares to the cached values. Returns nullopt on disagreement;
// caller must repair (clear_row+repaint) before retrying.
//
// damage_ is not re-derived here — we don't have the prior frame's cells
// available at this layer; that obligation is met by the existing shadow
// witness one layer up. T0's claim is restricted to last_col_/max_y_,
// which is the cache class that caused the ghost-row symptom.
[[nodiscard]] std::optional<CanvasWitness> verify_canvas(const Canvas& c) noexcept;

// Hash helpers used by both verify_canvas and the in-diff re-check.
[[nodiscard]] std::uint64_t hash_canvas_cells(const Canvas& c) noexcept;
[[nodiscard]] std::uint64_t hash_canvas_caches(const Canvas& c) noexcept;

} // namespace maya
