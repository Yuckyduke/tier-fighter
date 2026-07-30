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

// --- Transition blending -----------------------------------------------------
// Without blending, a state change snaps instantly from one pose to another: idle's
// arms are at 12 degrees, dash wants 46, and the figure teleports between them on a
// single frame. That reads as a glitch even when both poses are individually fine.
//
// The fix is a short generated bridge. On a state change we capture the pose the
// figure was ACTUALLY in -- mid-animation, whatever it happened to be -- and ease
// from that into the new animation over a few frames.
//
// Generated rather than authored, deliberately. 42 states means over 1700 ordered
// pairs; hand-making bridges for those is not a real option, and most would be
// near-identical eases anyway. Capturing the live pose also handles the case a
// hand-authored bridge cannot: leaving a state part-way through, where the starting
// pose depends on when you left.
//
// Blender is stateful, so it lives with the caller (one per player, one in the
// editor) rather than being a global -- the game has several fighters transitioning
// independently.
struct Blender {
    Pose from{}; // pose we were in when the state changed
    ActionState lastState{};
    int blendFrame = 0;  // frames into the current blend
    int blendLen = 0;    // 0 = not blending
    bool primed = false; // false until the first pose is seen

    // Per-state blend lengths would be per-pair in the ideal case, but a single
    // duration plus an ease curve gets most of the benefit. Short enough not to feel
    // laggy on fast actions, long enough to hide a large pose jump.
    int defaultBlendFrames = 5;
};

// The blended pose. Call once per frame per figure, in order -- it detects the state
// change itself by comparing against the previous call.
Pose poseBlended(Blender &b, ActionState st, int stateFrame, uint8_t attackId);

// How far apart two poses are, in degrees summed across the joints. Used to scale
// blend length: a small change needs no bridge, a large one needs a longer ease.
float poseDistance(const Pose &a, const Pose &b);

// --- Sequences ---------------------------------------------------------------
// A pose in isolation says little; what usually reads badly is the TRANSITION
// between two states. A sequence chains states back-to-back, each held for its own
// natural duration, so the editor can play a realistic action and expose the seams:
// a lean that snaps, a figure that pops up too fast, limbs that jump between poses.
//
// These mirror sequences a player actually performs, not arbitrary orderings.
constexpr int kMaxSeqSteps = 8;

struct Sequence {
    const char *name;
    ActionState steps[kMaxSeqSteps];
    int count;
};

int sequenceCount();
const Sequence &sequenceAt(int i);

// Which step of a sequence is playing at `frame`, and how far into that step we are.
// Returns false once the sequence has finished.
bool sequenceSample(const Sequence &seq, int frame, int *outStep, int *outStepFrame);

// How long a state's animation runs for, used by both Lerp timing and sequences.
int stateDuration(ActionState st);

// Editable table, indexed by ActionState. Exposed so the editor can mutate it live.
Anim &animFor(ActionState st);
const char *stateLabel(ActionState st);
int stateCount();
ActionState stateByIndex(int i);

} // namespace tf::pose
