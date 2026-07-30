#pragma once
#include "sim/state.h"

// Stick-figure skeleton and pose data.
//
// This is PRESENTATION ONLY -- nothing here feeds back into the simulation. Poses
// use plain floats and degrees because they are read by the renderer, never by
// step(). (Contrast sim/config.h, where everything must be fixed-point.)
//
// Why poses rather than sprite frames: a stick figure is procedural. One pose is
// eleven numbers on a single line, so authoring all forty states is a page of data
// instead of hundreds of hand-drawn images. Interpolating between a start and end
// pose over a state's duration gives motion for free, and because it reads the same
// stateFrame the simulation uses, the animation is automatically frame-accurate --
// you can SEE startup vs active vs recovery.

namespace tf::pose {

// Joint angles in degrees, measured from straight down, positive = clockwise when
// facing right. The renderer mirrors everything horizontally for facing left, so
// each pose is authored once for a right-facing fighter.
//
// Lengths are fractions of body height so a pose fits any character size.
struct Pose {
    float leanDeg;     // torso tilt from vertical
    float headOffsetY; // head raise/lower, fraction of height (crouching)

    float shoulderFrontDeg; // front arm, from the shoulder
    float elbowFrontDeg;    // front elbow bend (0 = straight)
    float shoulderBackDeg;
    float elbowBackDeg;

    float hipFrontDeg;  // front leg, from the hip
    float kneeFrontDeg; // front knee bend
    float hipBackDeg;
    float kneeBackDeg;

    float crouch; // 0 = standing, 1 = fully compressed
};

// A state's animation: hold `a` if there is no `b`, otherwise interpolate a -> b
// across `frames` (or the state's own duration when `frames` is 0).
struct Anim {
    Pose a;
    Pose b;
    int frames; // 0 = use the state's natural duration
    bool loop;  // ping-pong rather than settling on b
    bool hasB;
};

// The pose for a given state and frame. Pure function of the two, so the editor and
// the game cannot disagree about what a state looks like.
Pose poseFor(ActionState st, int stateFrame, uint8_t attackId);

// Editable table, indexed by ActionState. Exposed so the editor can mutate it live.
Anim &animFor(ActionState st);
const char *stateLabel(ActionState st);
int stateCount();
ActionState stateByIndex(int i);

} // namespace tf::pose
