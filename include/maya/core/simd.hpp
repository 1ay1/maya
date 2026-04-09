#pragma once
// maya::simd - SIMD-accelerated bulk operations for terminal cells
//
// Hardware-accelerated comparison of packed 64-bit cell arrays. Uses
// AVX-512F (8 cells/cycle), AVX2 (4 cells/cycle), SSE2 (2 cells/cycle,
// x86-64 baseline), or NEON (2 cells/cycle, ARM64). Scalar fallback for
// everything else.
//
// Implementation lives in platform/simd/ — each ISA has its own header
// with an Ops<Isa> specialization. This header simply re-exports the
// public API from platform/simd/dispatch.hpp.

#include "../platform/simd/dispatch.hpp"
