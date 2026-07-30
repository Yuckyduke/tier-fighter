#pragma once
#include "fixed.h"

namespace tf {

// Baked sin table in Q16.16, one entry per whole degree, indices 0..360.
// Defined in trig.cpp. See that file for why it is hardcoded.
extern const int32_t kSinTable[361];

// Degree-resolution sin/cos. Knockback angles are authored in whole degrees, so
// one entry per degree is all the simulation needs.
inline fx fx_sin_deg(int32_t deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    return static_cast<fx>(kSinTable[deg]);
}

inline fx fx_cos_deg(int32_t deg) { return fx_sin_deg(deg + 90); }

}  // namespace tf
