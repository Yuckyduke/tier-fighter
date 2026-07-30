#pragma once
#include "sim/state.h"
#include "skeleton.h"

#include <raylib.h>

// Draw a stick figure from a pose. Presentation only -- see skeleton.h.

namespace tf::stick {

// Screen-space geometry for one figure, so the caller controls placement and scale
// rather than this depending on the game's camera.
struct Frame {
    float footX;      // where the feet touch
    float footY;
    float height;     // full standing height in pixels
    int   facing;     // +1 right, -1 left
};

void drawPose(const pose::Pose& p, const Frame& f, Color body, float thickness);

// Convenience: resolve the pose for a state/frame and draw it.
void drawState(ActionState st, int stateFrame, uint8_t attackId,
               const Frame& f, Color body, float thickness);

}  // namespace tf::stick
