#pragma once
#include <cstdint>

// Q16.16 fixed-point math.
//
// Why not float? Rollback netcode re-simulates past frames and requires every
// machine to produce bit-identical results. Floats do not guarantee that: results
// drift across compilers, optimization levels (-ffast-math especially), x87 vs
// SSE, and x86 vs ARM. One differing bit compounds into a visible desync.
// Integers are exact everywhere, so the simulation uses only these.
//
// Range: +/-32767.99998, resolution 1/65536. Positions are in world units
// (~1 unit = 1 pixel), so this is comfortable headroom for a fighting stage.

namespace tf {

using fx = int32_t;

constexpr int kFxShift = 16;
constexpr fx  kFxOne   = 1 << kFxShift;
constexpr fx  kFxHalf  = kFxOne / 2;

// Integer -> fixed. Multiplies rather than shifts: shifting a negative left is
// undefined behavior before C++20, multiplication is always well defined.
constexpr fx fxi(int32_t v) { return static_cast<fx>(v * kFxOne); }

// Exact rational constant, e.g. fx_ratio(9, 10) == 0.9. This is how all tuning
// constants are written -- there is deliberately no float constructor, so no
// float can leak into the simulation by accident.
constexpr fx fx_ratio(int32_t num, int32_t den) {
    return static_cast<fx>((static_cast<int64_t>(num) * kFxOne) / den);
}

// 64-bit intermediate prevents overflow on the product before the shift.
constexpr fx fx_mul(fx a, fx b) {
    return static_cast<fx>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> kFxShift);
}

constexpr fx fx_div(fx a, fx b) {
    if (b == 0) return 0;  // Defined result beats UB: a desync is worse than a wrong number.
    return static_cast<fx>((static_cast<int64_t>(a) << kFxShift) / b);
}

constexpr int32_t fx_to_int(fx v)   { return v >> kFxShift; }   // floors toward -inf
constexpr fx      fx_abs(fx v)      { return v < 0 ? -v : v; }
constexpr fx      fx_min(fx a, fx b){ return a < b ? a : b; }
constexpr fx      fx_max(fx a, fx b){ return a > b ? a : b; }
constexpr fx      fx_clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Move `v` toward zero by `amount` without overshooting past it.
constexpr fx fx_decay(fx v, fx amount) {
    if (v > 0) return v - amount > 0 ? v - amount : 0;
    if (v < 0) return v + amount < 0 ? v + amount : 0;
    return 0;
}

// Squared magnitude, kept in 64-bit.
//
// fx_mul returns int32_t, so squaring a large distance OVERFLOWS and wraps
// negative -- at 182 world units apart, fx_mul(d,d) goes negative and any
// "is it smaller than the radius" test reports a hit. That produced phantom hits
// from across the stage.
//
// These helpers never round-trip through fx, so there is nothing to overflow.
// Use them for ANY distance comparison; fx_mul is only safe for small values
// like velocities.
constexpr int64_t fx_len_sq(fx x, fx y) {
    const int64_t xx = static_cast<int64_t>(x);
    const int64_t yy = static_cast<int64_t>(y);
    return ((xx * xx) >> kFxShift) + ((yy * yy) >> kFxShift);
}

constexpr int64_t fx_sq(fx v) {
    const int64_t vv = static_cast<int64_t>(v);
    return (vv * vv) >> kFxShift;
}

// Integer square root, digit-by-digit. Deterministic on every platform, unlike
// std::sqrt which is only *usually* exact. Used to normalize the analog stick
// for directional influence.
constexpr fx fx_sqrt(fx v) {
    if (v <= 0) return 0;
    // Work in Q32.32 so the result lands back in Q16.16 after the root.
    uint64_t n = static_cast<uint64_t>(v) << kFxShift;
    uint64_t res = 0;
    uint64_t bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<fx>(res);
}

// Rendering only -- never call this inside the simulation.
inline float fx_to_float(fx v) { return static_cast<float>(v) / static_cast<float>(kFxOne); }

}  // namespace tf
