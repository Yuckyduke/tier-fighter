#pragma once
#include <cstdint>

namespace tf {

enum Button : uint16_t {
    BtnJump   = 1u << 0,
    BtnAttack = 1u << 1,
    BtnShield = 1u << 2,  // held to air dodge; shielding itself is post-MVP
};

// One frame of input for one player. Deliberately tiny and POD: this is the unit
// that gets sent over the network, stored in a ring buffer, and predicted during
// rollback. Analog stick is -100..100 so it serializes exactly with no float.
struct Input {
    uint16_t buttons = 0;
    int8_t   stickX  = 0;
    int8_t   stickY  = 0;

    bool held(Button b)    const { return (buttons & b) != 0; }
    // True when `b` is down now and was up last frame.
    bool pressed(Button b, const Input& prev) const {
        return (buttons & b) && !(prev.buttons & b);
    }
};

// Stick magnitude past which we treat an axis as intentionally deflected.
constexpr int8_t kStickDeadzone = 28;
// Stick deflection required for a "hard" input (dash, fast fall).
constexpr int8_t kStickHard = 62;

}  // namespace tf
