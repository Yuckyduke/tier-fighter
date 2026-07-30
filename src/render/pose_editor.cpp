#include "sim/state.h"
#include "skeleton.h"
#include "stickfigure.h"

#include <raylib.h>

#include <cstdio>

// Standalone pose editor.
//
// Deliberately separate from the game: no opponent, no physics, no arena. One
// figure, one state at a time, large and centred, so the only question on screen is
// "does this pose read correctly?".
//
// The pose table lives in skeleton.cpp and is shared with the game, so anything
// tuned here immediately improves the real thing. The workflow is: page through the
// states, find one that reads badly, adjust its angles live, then write the numbers
// back into the table.

using namespace tf;

namespace {

constexpr int kScreenW = 1280;
constexpr int kScreenH = 800;

// Which field the arrow keys currently edit.
struct Field {
    const char *name;
    float pose::Pose::*ptr;
    float step;
};

const Field kFields[] = {
    {"lean", &pose::Pose::leanDeg, 2.0f},
    {"head Y", &pose::Pose::headOffsetY, 0.01f},
    {"crouch", &pose::Pose::crouch, 0.02f},
    {"shoulder F", &pose::Pose::shoulderFrontDeg, 2.0f},
    {"elbow F", &pose::Pose::elbowFrontDeg, 2.0f},
    {"shoulder B", &pose::Pose::shoulderBackDeg, 2.0f},
    {"elbow B", &pose::Pose::elbowBackDeg, 2.0f},
    {"hip F", &pose::Pose::hipFrontDeg, 2.0f},
    {"knee F", &pose::Pose::kneeFrontDeg, 2.0f},
    {"hip B", &pose::Pose::hipBackDeg, 2.0f},
    {"knee B", &pose::Pose::kneeBackDeg, 2.0f},
};
constexpr int kFieldCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));

// Print a pose in exactly the P(...) form used in skeleton.cpp, so an edit can be
// pasted straight back into the table.
void printPose(const char *label, const pose::Pose &p) {
    std::printf("    // %s\n    P(%g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g)\n", label, p.leanDeg,
                p.headOffsetY, p.shoulderFrontDeg, p.elbowFrontDeg, p.shoulderBackDeg,
                p.elbowBackDeg, p.hipFrontDeg, p.kneeFrontDeg, p.hipBackDeg, p.kneeBackDeg,
                p.crouch);
}

} // namespace

int main() {
    InitWindow(kScreenW, kScreenH, "tier-fighter -- pose editor");
    SetTargetFPS(60);

    // Two modes. SINGLE isolates one state for editing angles; SEQUENCE chains
    // states so transitions are visible -- which is where problems usually are, since
    // a pose can look fine alone and still snap badly into its neighbour.
    enum class Mode { Single, Sequence };
    Mode mode = Mode::Single;

    int stateIdx = 0;
    int seqIdx = 0;
    int field = 0;
    int frame = 0;
    bool playing = true;
    bool loopSeq = true;
    // Trail of recent positions, so fast motion is legible rather than a blur.
    struct Ghost {
        pose::Pose pose;
        int age;
    };
    Ghost ghosts[10] = {};
    int ghostCount = 0;
    int ghostTimer = 0;

    // Transition blending. A toggle rather than always-on, because seeing the
    // unblended version is how you tell whether a bridge is actually needed or
    // whether the underlying poses are simply wrong.
    pose::Blender blender;
    bool blendOn = true;
    bool editingB = false; // which keyframe of the animation is being edited
    bool showGrid = true;
    bool sideBySide = true; // draw start and end poses next to the animation
    float scale = 1.0f;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_M)) {
            mode = (mode == Mode::Single) ? Mode::Sequence : Mode::Single;
            frame = 0;
            ghostCount = 0;
        }

        // In sequence mode the displayed state is whichever step is playing, so the
        // editor always edits the pose you are actually looking at.
        const pose::Sequence &seq = pose::sequenceAt(seqIdx);
        int seqStep = 0, seqStepFrame = 0;
        bool seqRunning = true;
        if (mode == Mode::Sequence) {
            seqRunning = pose::sequenceSample(seq, frame, &seqStep, &seqStepFrame);
            if (!seqRunning && loopSeq) {
                frame = 0;
                pose::sequenceSample(seq, frame, &seqStep, &seqStepFrame);
                seqRunning = true;
            }
        }

        const ActionState st =
            (mode == Mode::Sequence) ? seq.steps[seqStep] : pose::stateByIndex(stateIdx);
        pose::Anim &an = pose::animFor(st);
        const int animFrame = (mode == Mode::Sequence) ? seqStepFrame : frame;

        // --- Selection -------------------------------------------------------
        if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_N)) {
            if (mode == Mode::Sequence) {
                seqIdx = (seqIdx + 1) % pose::sequenceCount();
            } else {
                stateIdx = (stateIdx + 1) % pose::stateCount();
            }
            frame = 0;
            ghostCount = 0;
        }
        if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_P)) {
            if (mode == Mode::Sequence) {
                seqIdx = (seqIdx - 1 + pose::sequenceCount()) % pose::sequenceCount();
            } else {
                stateIdx = (stateIdx - 1 + pose::stateCount()) % pose::stateCount();
            }
            frame = 0;
            ghostCount = 0;
        }
        if (IsKeyPressed(KEY_L)) loopSeq = !loopSeq;
        if (IsKeyPressed(KEY_B)) {
            blendOn = !blendOn;
            blender = pose::Blender{}; // reset so the next frame re-primes cleanly
        }
        if (IsKeyPressed(KEY_LEFT_ALT)) {
            // Cycle blend length, so you can feel how long is too long.
            blender.defaultBlendFrames =
                (blender.defaultBlendFrames >= 12) ? 2 : blender.defaultBlendFrames + 2;
        }

        // --- Field selection and editing -------------------------------------
        if (IsKeyPressed(KEY_DOWN)) field = (field + 1) % kFieldCount;
        if (IsKeyPressed(KEY_UP)) field = (field - 1 + kFieldCount) % kFieldCount;

        pose::Pose &edited = editingB ? an.b : an.a;
        const Field &fd = kFields[field];
        const float mult = IsKeyDown(KEY_LEFT_SHIFT) ? 4.0f : 1.0f;
        if (IsKeyDown(KEY_LEFT)) edited.*(fd.ptr) -= fd.step * mult;
        if (IsKeyDown(KEY_RIGHT)) edited.*(fd.ptr) += fd.step * mult;

        if (IsKeyPressed(KEY_TAB)) {
            editingB = !editingB;
            if (editingB) an.hasB = true;
        }

        // --- Playback --------------------------------------------------------
        if (IsKeyPressed(KEY_SPACE)) playing = !playing;
        if (IsKeyPressed(KEY_COMMA)) {
            frame = frame > 0 ? frame - 1 : 0;
            playing = false;
        }
        if (IsKeyPressed(KEY_PERIOD)) {
            ++frame;
            playing = false;
        }
        if (IsKeyPressed(KEY_R)) frame = 0;
        if (playing) ++frame;

        // Capture a ghost every few frames while playing. Fast actions (a dash, a
        // roll) are otherwise a blur; the trail makes the arc of motion visible.
        if (playing && ++ghostTimer >= 4) {
            ghostTimer = 0;
            for (int i = 9; i > 0; --i)
                ghosts[i] = ghosts[i - 1];
            ghosts[0] = Ghost{blendOn ? blender.from : pose::poseFor(st, animFrame, 0), 0};
            if (ghostCount < 10) ++ghostCount;
        }

        if (IsKeyPressed(KEY_G)) showGrid = !showGrid;
        if (IsKeyPressed(KEY_S)) sideBySide = !sideBySide;
        if (IsKeyPressed(KEY_EQUAL)) scale += 0.1f;
        if (IsKeyPressed(KEY_MINUS)) scale = scale > 0.4f ? scale - 0.1f : scale;

        // Dump the current pose to stdout, ready to paste into skeleton.cpp.
        if (IsKeyPressed(KEY_D)) {
            std::printf("\n// --- %s (%s keyframe) ---\n", pose::stateLabel(st),
                        editingB ? "b" : "a");
            printPose("a", an.a);
            if (an.hasB) printPose("b", an.b);
            std::fflush(stdout);
        }

        // --- Draw ------------------------------------------------------------
        BeginDrawing();
        ClearBackground(Color{20, 22, 28, 255});

        const float groundY = kScreenH * 0.78f;
        const float figH = kScreenH * 0.46f * scale;

        if (showGrid) {
            // Ground line plus a height reference, so proportions stay consistent
            // as poses are edited.
            DrawLineEx(Vector2{0, groundY}, Vector2{kScreenW, groundY}, 2.0f,
                       Color{60, 66, 78, 255});
            for (int i = 1; i <= 4; ++i) {
                const float y = groundY - figH * (static_cast<float>(i) / 4.0f);
                DrawLineEx(Vector2{0, y}, Vector2{kScreenW, y}, 1.0f, Color{38, 42, 50, 255});
            }
        }

        const float cx = kScreenW * 0.5f;

        // Motion trail, oldest and faintest first.
        for (int i = ghostCount - 1; i >= 1; --i) {
            const float fade = 1.0f - static_cast<float>(i) / 10.0f;
            const unsigned char alpha = static_cast<unsigned char>(50.0f * fade);
            stick::Frame gf{cx - static_cast<float>(i) * figH * 0.035f, groundY, figH, 1};
            stick::drawPose(ghosts[i].pose, gf, Color{90, 140, 190, alpha}, figH * 0.02f);
        }

        // The animated figure, centre stage. Resolved through the blender so state
        // changes ease rather than snap -- the whole point of sequence mode is seeing
        // that seam, so it has to be rendered the way the game will render it.
        const pose::Pose shown = blendOn ? pose::poseBlended(blender, st, animFrame, 0)
                                         : pose::poseFor(st, animFrame, 0);
        stick::Frame mainFrame{cx, groundY, figH, 1};
        stick::drawPose(shown, mainFrame, Color{120, 200, 255, 255}, figH * 0.028f);

        // Keyframes either side, dimmed, so you can see where the motion starts and
        // ends without scrubbing back and forth.
        if (sideBySide && an.hasB) {
            stick::Frame aF{kScreenW * 0.18f, groundY, figH * 0.8f, 1};
            stick::Frame bF{kScreenW * 0.82f, groundY, figH * 0.8f, 1};
            stick::drawPose(an.a, aF, Color{90, 110, 130, 200}, figH * 0.022f);
            stick::drawPose(an.b, bF, Color{130, 100, 130, 200}, figH * 0.022f);
            DrawText("keyframe a", static_cast<int>(kScreenW * 0.18f) - 40,
                     static_cast<int>(groundY) + 12, 16, Color{110, 130, 150, 255});
            DrawText("keyframe b", static_cast<int>(kScreenW * 0.82f) - 40,
                     static_cast<int>(groundY) + 12, 16, Color{150, 120, 150, 255});
        }

        // A left-facing copy, to confirm mirroring reads correctly.
        stick::Frame mirror{kScreenW * 0.5f + figH * 0.75f, groundY, figH * 0.55f, -1};
        stick::drawPose(shown, mirror, Color{80, 90, 105, 180}, figH * 0.018f);

        // --- HUD -------------------------------------------------------------
        char line[256];
        if (mode == Mode::Sequence) {
            std::snprintf(line, sizeof(line), "SEQUENCE: %s   (%d/%d)", seq.name, seqIdx + 1,
                          pose::sequenceCount());
        } else {
            std::snprintf(line, sizeof(line), "%s   (%d/%d)", pose::stateLabel(st), stateIdx + 1,
                          pose::stateCount());
        }
        DrawText(line, 24, 20, 30, Color{230, 235, 245, 255});

        std::snprintf(line, sizeof(line), "frame %d   %s   editing keyframe %s   %s%s", frame,
                      playing ? "playing" : "PAUSED", editingB ? "B" : "A",
                      an.hasB ? (an.loop ? "cycle" : "lerp") : "hold",
                      (mode == Mode::Sequence && loopSeq) ? "   [looping]" : "");
        DrawText(line, 24, 56, 18, Color{160, 175, 195, 255});

        std::snprintf(line, sizeof(line), "blend %s  (%d fr)%s", blendOn ? "ON" : "off",
                      blender.defaultBlendFrames, blender.blendLen > 0 ? "   BLENDING" : "");
        DrawText(line, kScreenW - 260, 22, 18,
                 blender.blendLen > 0 ? Color{255, 220, 120, 255} : Color{130, 145, 165, 255});

        // In sequence mode, show the chain with the active step highlighted -- so it
        // is obvious WHICH transition you are looking at when something snaps.
        if (mode == Mode::Sequence) {
            int tx = 24;
            const int ty = 80;
            for (int i = 0; i < seq.count; ++i) {
                const bool active = (i == seqStep);
                const char *lbl = pose::stateLabel(seq.steps[i]);
                std::snprintf(line, sizeof(line), "%s%s", lbl, i + 1 < seq.count ? "  >  " : "");
                DrawText(line, tx, ty, 17,
                         active ? Color{255, 220, 120, 255} : Color{110, 122, 138, 255});
                tx += MeasureText(line, 17);
            }
        }

        // Field list, current one highlighted.
        int y = (mode == Mode::Sequence) ? 116 : 100;
        for (int i = 0; i < kFieldCount; ++i) {
            const bool sel = (i == field);
            std::snprintf(line, sizeof(line), "%s %-11s %7.2f", sel ? ">" : " ", kFields[i].name,
                          edited.*(kFields[i].ptr));
            DrawText(line, 24, y, 18, sel ? Color{255, 220, 120, 255} : Color{130, 140, 155, 255});
            y += 22;
        }

        const char *help = "M  single <-> SEQUENCE mode   L  loop   B  blending on/off\n"
                           "ALT  cycle blend length\n"
                           "[ ]  prev/next state or sequence   UP/DOWN  select field\n"
                           "LEFT/RIGHT  adjust (hold SHIFT for x4)\n"
                           "TAB  switch keyframe A/B   SPACE  play/pause\n"
                           ",  .  step frame           R  restart\n"
                           "D  dump pose to stdout     G  grid   S  keyframes\n"
                           "+ -  zoom";
        DrawText(help, 24, kScreenH - 132, 16, Color{120, 130, 145, 255});

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
