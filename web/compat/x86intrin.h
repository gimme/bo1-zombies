// MSVC <intrin.h>: the SSE/SSE2 intrinsics (Emscripten maps them onto wasm SIMD) plus the MSVC-only ones in
// bo1_web_prelude.h.
#pragma once
#include "bo1_web_prelude.h"
#include <xmmintrin.h>
#include <emmintrin.h>
