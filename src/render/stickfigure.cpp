#include "stickfigure.h"

#include <cmath>

namespace tf::stick {

namespace {

// Proportions as fractions of standing height. Roughly heroic-figure ratios rather
// than realistic ones -- a slightly large head and long limbs read better at small
// sizes and telegraph poses more clearly.
constexpr float kHeadRadius = 0.115f;
constexpr float kNeckY = 0.78f; // measured up from the feet
constexpr float kHipY = 0.46f;
constexpr float kShoulderY = 0.72f;
constexpr float kUpperArm = 0.19f;
constexpr float kForeArm = 0.18f;
constexpr float kThigh = 0.24f;
constexpr float kShin = 0.22f;

// Angles are measured from straight DOWN, positive clockwise for a right-facing
// figure. Screen Y grows downward, which is why sin/cos land this way.
Vector2 offsetFrom(Vector2 origin, float deg, float len, int facing) {
    const float r = deg * 3.14159265f / 180.0f;
    return Vector2{origin.x + std::sin(r) * len * static_cast<float>(facing),
                   origin.y + std::cos(r) * len};
}

} // namespace

void drawPose(const pose::Pose &p, const Frame &f, Color body, float thickness) {
    const float h = f.height;
    const int dir = f.facing >= 0 ? 1 : -1;

    // Crouching compresses the whole figure toward the feet.
    const float squash = 1.0f - p.crouch * 0.42f;

    // Hip sits above the feet, lowered by the crouch.
    const Vector2 hip{f.footX, f.footY - kHipY * h * squash};

    // Torso leans from the hip.
    const float leanR = p.leanDeg * 3.14159265f / 180.0f;
    const float torsoLen = (kNeckY - kHipY) * h * squash;
    const Vector2 neck{hip.x + std::sin(leanR) * torsoLen * static_cast<float>(dir),
                       hip.y - std::cos(leanR) * torsoLen};

    const float shoulderLen = (kShoulderY - kHipY) * h * squash;
    const Vector2 shoulder{hip.x + std::sin(leanR) * shoulderLen * static_cast<float>(dir),
                           hip.y - std::cos(leanR) * shoulderLen};

    // Head rides above the neck, offset for crouching/tucking.
    const float headR = kHeadRadius * h;
    const Vector2 head{neck.x + std::sin(leanR) * headR * static_cast<float>(dir),
                       neck.y - std::cos(leanR) * headR + p.headOffsetY * h};

    // Limbs. Each is two segments so the joint bend is visible -- a straight arm
    // and a bent one read very differently, and that difference is most of what
    // makes a pose legible.
    const float upper = kUpperArm * h;
    const float fore = kForeArm * h;
    const float thigh = kThigh * h * squash;
    const float shin = kShin * h * squash;

    const Vector2 elbowB = offsetFrom(shoulder, p.shoulderBackDeg, upper, dir);
    const Vector2 handB = offsetFrom(elbowB, p.shoulderBackDeg + p.elbowBackDeg, fore, dir);
    const Vector2 kneeB = offsetFrom(hip, p.hipBackDeg, thigh, dir);
    const Vector2 footB = offsetFrom(kneeB, p.hipBackDeg + p.kneeBackDeg, shin, dir);

    const Vector2 elbowF = offsetFrom(shoulder, p.shoulderFrontDeg, upper, dir);
    const Vector2 handF = offsetFrom(elbowF, p.shoulderFrontDeg + p.elbowFrontDeg, fore, dir);
    const Vector2 kneeF = offsetFrom(hip, p.hipFrontDeg, thigh, dir);
    const Vector2 footF = offsetFrom(kneeF, p.hipFrontDeg + p.kneeFrontDeg, shin, dir);

    // Back limbs are dimmed so the figure reads as three-dimensional rather than
    // as a tangle of identical lines.
    const Color backColor{static_cast<unsigned char>(body.r / 2 + 20),
                          static_cast<unsigned char>(body.g / 2 + 20),
                          static_cast<unsigned char>(body.b / 2 + 20), body.a};

    DrawLineEx(shoulder, elbowB, thickness * 0.8f, backColor);
    DrawLineEx(elbowB, handB, thickness * 0.8f, backColor);
    DrawLineEx(hip, kneeB, thickness * 0.9f, backColor);
    DrawLineEx(kneeB, footB, thickness * 0.9f, backColor);

    DrawLineEx(hip, neck, thickness * 1.15f, body);

    DrawLineEx(hip, kneeF, thickness, body);
    DrawLineEx(kneeF, footF, thickness, body);
    DrawLineEx(shoulder, elbowF, thickness * 0.9f, body);
    DrawLineEx(elbowF, handF, thickness * 0.9f, body);

    DrawCircleV(head, headR, body);
    // A small facing dot, so which way the figure looks is unambiguous even in a
    // symmetrical pose.
    DrawCircleV(Vector2{head.x + headR * 0.42f * static_cast<float>(dir), head.y - headR * 0.12f},
                headR * 0.17f, Color{20, 22, 28, 255});
}

void drawState(ActionState st, int stateFrame, uint8_t attackId, const Frame &f, Color body,
               float thickness) {
    drawPose(pose::poseFor(st, stateFrame, attackId), f, body, thickness);
}

} // namespace tf::stick
