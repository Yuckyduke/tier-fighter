#include "sim/config.h"
#include "sim/state.h"
#include "skeleton.h"
#include "stickfigure.h"

#include <raylib.h>

#include <cstdio>
#include <cstring>

// Local playtest client: 2 players, one stage, no networking.
//
// This is a DEBUG renderer, deliberately ugly. Rectangles and text, no sprites,
// no animation. The only question it exists to answer is whether the movement
// feels good -- every constant in config.h is a guess, and guesses can only be
// judged with hands on the controls.
//
// The renderer is a READ-ONLY OBSERVER. It reads GameState and paints; it never
// writes to it. That boundary is why the same simulation compiles into a headless
// server unchanged, and why rollback works: when the network says "rewind 4 frames
// and re-simulate", the sim silently re-runs and the renderer just draws whatever
// the corrected state is.
//
// Floats appear freely in this file. That is fine and expected -- fx_to_float is
// for exactly this. Nobody cares if a pixel lands 0.001 off. What matters is that
// no float ever flows back INTO the simulation.

using namespace tf;

namespace {

constexpr int kScreenW = 1400;
constexpr int kScreenH = 900;

// --- World -> screen ---------------------------------------------------------
// Fixed camera framing the whole blast-zone region. No follow-cam: for tuning you
// want to see players fly off and where they die, not stay centered on them.
struct ViewTransform {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    float sx(fx worldX) const { return fx_to_float(worldX) * scale + offsetX; }
    float sy(fx worldY) const { return fx_to_float(worldY) * scale + offsetY; }
    float len(fx worldLen) const { return fx_to_float(worldLen) * scale; }
};

ViewTransform fitStage(const Stage &s, int screenW, int screenH) {
    const float margin = 60.0f;
    const float left = fx_to_float(s.blastLeft) - margin;
    const float right = fx_to_float(s.blastRight) + margin;
    const float top = fx_to_float(s.blastTop) - margin;
    const float bottom = fx_to_float(s.blastBottom) + margin;

    const float worldW = right - left;
    const float worldH = bottom - top;

    ViewTransform v;
    const float sx = static_cast<float>(screenW) / worldW;
    const float sy = static_cast<float>(screenH) / worldH;
    v.scale = sx < sy ? sx : sy;
    v.offsetX = (static_cast<float>(screenW) - worldW * v.scale) * 0.5f - left * v.scale;
    v.offsetY = (static_cast<float>(screenH) - worldH * v.scale) * 0.5f - top * v.scale;
    return v;
}

// --- Input ------------------------------------------------------------------
// Keyboard and gamepad are merged. The float->int8 conversion here is NOT a
// determinism leak: the stick is quantized to an integer BEFORE entering the
// simulation, and that quantized value is exactly what would go over the network.
int8_t quantizeAxis(float raw) {
    const float deadzone = 0.15f;
    if (raw > -deadzone && raw < deadzone) return 0;
    float v = raw * 100.0f;
    if (v > 100.0f) v = 100.0f;
    if (v < -100.0f) v = -100.0f;
    return static_cast<int8_t>(v);
}

struct KeyBinding {
    int left, right, up, down;
    int jump, attack, shield;
    int gamepad;
};

constexpr KeyBinding kP1{
    KEY_A, KEY_D, KEY_W, KEY_S, KEY_SPACE, KEY_F, KEY_G, 0,
};

constexpr KeyBinding kP2{
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_RIGHT_SHIFT, KEY_PERIOD, KEY_SLASH, 1,
};

Input readInput(const KeyBinding &b) {
    Input in;

    int8_t sx = 0, sy = 0;
    if (IsKeyDown(b.left)) sx = -100;
    if (IsKeyDown(b.right)) sx = 100;
    // +Y is DOWN in the simulation, matching screen convention.
    if (IsKeyDown(b.up)) sy = -100;
    if (IsKeyDown(b.down)) sy = 100;

    uint16_t buttons = 0;
    if (IsKeyDown(b.jump)) buttons |= BtnJump;
    if (IsKeyDown(b.attack)) buttons |= BtnAttack;
    if (IsKeyDown(b.shield)) buttons |= BtnShield;

    // Gamepad overrides the stick when deflected, and ORs its buttons in.
    if (IsGamepadAvailable(b.gamepad)) {
        const int8_t gx = quantizeAxis(GetGamepadAxisMovement(b.gamepad, GAMEPAD_AXIS_LEFT_X));
        const int8_t gy = quantizeAxis(GetGamepadAxisMovement(b.gamepad, GAMEPAD_AXIS_LEFT_Y));
        if (gx != 0) sx = gx;
        if (gy != 0) sy = gy;

        if (IsGamepadButtonDown(b.gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) buttons |= BtnJump;
        if (IsGamepadButtonDown(b.gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) buttons |= BtnAttack;
        if (IsGamepadButtonDown(b.gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) buttons |= BtnShield;
        if (IsGamepadButtonDown(b.gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) buttons |= BtnShield;
    }

    in.stickX = sx;
    in.stickY = sy;
    in.buttons = buttons;
    return in;
}

// --- Per-character lookup ---------------------------------------------------
// Character constants are per-player now, so the renderer must resolve them from
// the player it is drawing rather than a global. Bounds-checked for the same
// reason the simulation checks: charId can arrive from the network.
const config::Fighter &fighterFor(const Player &p) {
    return config::kFighters[p.charId < config::CHAR_COUNT ? p.charId : config::CHAR_SCOUT];
}

// --- Debug labels -----------------------------------------------------------

// Which move is out. Essential for verifying the input matrix: if you flick side
// and get "tilt S" instead of "SMASH S", the flick window is the thing to tune.
const char *attackName(uint8_t id) {
    switch (id) {
    case config::ATK_JAB:         return "jab";
    case config::ATK_TILT_SIDE:   return "tilt S";
    case config::ATK_TILT_UP:     return "tilt U";
    case config::ATK_TILT_DOWN:   return "tilt D";
    case config::ATK_SMASH_SIDE:  return "SMASH S";
    case config::ATK_SMASH_UP:    return "SMASH U";
    case config::ATK_SMASH_DOWN:  return "SMASH D";
    case config::ATK_AIR_NEUTRAL: return "nair";
    case config::ATK_AIR_FORWARD: return "fair";
    case config::ATK_AIR_BACK:    return "bair";
    case config::ATK_AIR_UP:      return "uair";
    case config::ATK_AIR_DOWN:    return "dair (spike)";
    case config::ATK_GETUP:       return "get-up attack";
    case config::ATK_LEDGE:       return "ledge attack";
    default:                      return "";
    }
}

const char *stateName(ActionState s) {
    switch (s) {
    case ActionState::Idle:         return "Idle";
    case ActionState::Walk:         return "Walk";
    case ActionState::Dash:         return "DASH";
    case ActionState::Run:          return "RUN";
    case ActionState::RunBrake:     return "brake";
    case ActionState::Turn:         return "turn";
    case ActionState::Jumpsquat:    return "Jumpsquat";
    case ActionState::Airborne:     return "Airborne";
    case ActionState::Landing:      return "Landing";
    case ActionState::AttackGround: return "AttackGround";
    case ActionState::AttackAir:    return "AttackAir";
    case ActionState::AirDodge:     return "AirDodge";
    case ActionState::Hitstun:      return "Hitstun";
    case ActionState::Dead:         return "Dead";
    case ActionState::Bounce:       return "BOUNCE";
    case ActionState::DownWait:     return "DOWN";
    case ActionState::GetUp:        return "GetUp";
    case ActionState::GetUpRoll:    return "Roll";
    case ActionState::GetUpAttack:  return "GetUpAtk";
    case ActionState::Tech:         return "TECH!";
    case ActionState::Grabbing:     return "GRAB!";
    case ActionState::GrabHold:     return "holding";
    case ActionState::Pummel:       return "pummel";
    case ActionState::Throwing:     return "THROWING";
    case ActionState::GrabRelease:  return "released";
    case ActionState::Grabbed:      return "GRABBED";
    case ActionState::Thrown:       return "THROWN";
    case ActionState::ShieldOn:     return "shield^";
    case ActionState::Shield:       return "SHIELD";
    case ActionState::ShieldOff:    return "shieldv";
    case ActionState::ShieldStun:   return "SHLDSTUN";
    case ActionState::ShieldBroken: return "BROKEN!";
    case ActionState::Dizzy:        return "DIZZY";
    case ActionState::RollForward:  return "roll >";
    case ActionState::RollBack:     return "roll <";
    case ActionState::SpotDodge:    return "spotdodge";
    case ActionState::FallHelpless: return "HELPLESS";
    case ActionState::LedgeHang:    return "LEDGE";
    case ActionState::LedgeClimb:   return "climb";
    case ActionState::LedgeRoll:    return "ledge roll";
    case ActionState::LedgeAttack:  return "ledge atk";
    case ActionState::LedgeJump:    return "ledge jump";
    }
    return "?";
}

// --- Drawing ----------------------------------------------------------------

void drawStage(const Stage &s, const ViewTransform &v) {
    // Blast zone boundary. Cross it and you lose a stock -- in arena mode that's
    // what ejects you to another stage.
    DrawRectangleLines(static_cast<int>(v.sx(s.blastLeft)), static_cast<int>(v.sy(s.blastTop)),
                       static_cast<int>(v.len(s.blastRight - s.blastLeft)),
                       static_cast<int>(v.len(s.blastBottom - s.blastTop)), Color{60, 20, 20, 255});

    // The platform. Drawn with visible thickness downward so the ledges read.
    const float px = v.sx(s.platformLeft);
    const float py = v.sy(s.groundY);
    const float pw = v.len(s.platformRight - s.platformLeft);
    DrawRectangleV(Vector2{px, py}, Vector2{pw, 14.0f}, Color{70, 78, 92, 255});
    DrawLineEx(Vector2{px, py}, Vector2{px + pw, py}, 2.0f, Color{140, 150, 170, 255});

    // Ledge grab boxes. Drawn so a missed grab is diagnosable rather than mysterious
    // -- you can see whether you were outside the box or approaching from the wrong
    // side. Uses the default fighter's reach; per-character values differ slightly.
    const auto &L = config::kFighters[config::CHAR_SCOUT].ledge;
    const fx edges[2] = {s.platformLeft, s.platformRight};
    for (int i = 0; i < 2; ++i) {
        // Only the OUTWARD half is grabbable: left ledge from the left, right from
        // the right. That asymmetry is why the box is offset rather than centered.
        const fx boxLeft = (i == 0) ? edges[i] - L.grabReachX : edges[i];
        DrawRectangleLines(
            static_cast<int>(v.sx(boxLeft)), static_cast<int>(v.sy(s.groundY - L.grabReachUp)),
            static_cast<int>(v.len(L.grabReachX)),
            static_cast<int>(v.len(L.grabReachUp + L.grabReachDown)), Color{90, 140, 200, 110});
    }
}

// One blender per player -- they transition independently, and a shared blender
// would bridge from whichever player happened to be drawn last.
pose::Blender gBlenders[kMaxPlayers];

void drawPlayer(const Player &p, int index, const ViewTransform &v, bool showBoxes,
                bool stickFigures) {
    if (!p.active || p.state == ActionState::Dead) return;

    const auto &body = fighterFor(p).body;

    const Color palette[2] = {
        Color{80, 160, 240, 255},
        Color{240, 120, 90, 255},
    };
    Color c = palette[index % 2];

    // Invulnerability is visible: arrivals and respawns get a grace window, and
    // during tuning you need to know why a hit did nothing.
    if (p.invulnFrames > 0) {
        c = Color{static_cast<unsigned char>(c.r / 2 + 100),
                  static_cast<unsigned char>(c.g / 2 + 100),
                  static_cast<unsigned char>(c.b / 2 + 100), 200};
    }
    // Hitstun reads differently again -- you're being launched, not in control.
    if (p.state == ActionState::Hitstun) { c = Color{250, 240, 120, 255}; }

    const float left = v.sx(p.x - body.halfWidth);
    const float top = v.sy(p.y - body.height);
    const float w = v.len(body.halfWidth * 2);
    const float h = v.len(body.height);

    if (stickFigures) {
        // The figure stands ON the collision box's base, and is drawn taller than
        // the box -- the box is a hurtbox, not a silhouette, so matching it exactly
        // would produce a squat figure. Feet at the box base keeps the visual
        // grounded where the simulation thinks the player is.
        const float footY = v.sy(p.y);
        const float figH = h * 1.6f;
        stick::Frame sf{v.sx(p.x), footY, figH, p.facing};

        // Resolved through this player's blender so state changes ease rather than
        // snap. attackFrame drives attack poses so charge holds read correctly --
        // a held smash freezes attackFrame, so the wind-up pose holds with it.
        const int animFrame =
            (p.state == ActionState::AttackGround || p.state == ActionState::AttackAir ||
             p.state == ActionState::GetUpAttack || p.state == ActionState::LedgeAttack)
                ? static_cast<int>(p.attackFrame)
                : static_cast<int>(p.stateFrame);

        const pose::Pose shown =
            pose::poseBlended(gBlenders[index], p.state, animFrame, p.attackId);
        stick::drawPose(shown, sf, c, figH * 0.030f);
    } else {
        DrawRectangleV(Vector2{left, top}, Vector2{w, h}, c);

        // Facing indicator: which way an attack will come out.
        const float cx = v.sx(p.x);
        const float cy = v.sy(p.y - body.height / 2);
        const float tipX = cx + v.len(body.halfWidth) * 1.6f * static_cast<float>(p.facing);
        DrawLineEx(Vector2{cx, cy}, Vector2{tipX, cy}, 3.0f, WHITE);
    }

    // Shield bubble, radius scaled by remaining health so the resource is legible
    // at a glance rather than only in the text HUD.
    if (p.state == ActionState::ShieldOn || p.state == ActionState::Shield ||
        p.state == ActionState::ShieldStun) {
        const fx maxHp = fighterFor(p).shield.maxHealth;
        const float frac = maxHp > 0 ? fx_to_float(p.shieldHealth) / fx_to_float(maxHp) : 0.0f;
        const float r = v.len(body.height) * (0.55f + 0.45f * frac);
        const float bx = v.sx(p.x);
        const float by = v.sy(p.y - body.height / 2);
        DrawCircleV(Vector2{bx, by}, r, Color{110, 180, 255, 70});
        DrawCircleLinesV(Vector2{bx, by}, r, Color{150, 210, 255, 200});
    }

    if (!showBoxes) return;

    // Hurtbox outline: the region attacks test against.
    DrawRectangleLines(static_cast<int>(left), static_cast<int>(top), static_cast<int>(w),
                       static_cast<int>(h), Color{120, 255, 140, 160});

    // Active hitbox, if any.
    //
    // NOTE the -1: sim.cpp resolves the attack and THEN increments attackFrame, so
    // by the time we draw, attackFrame is one past the frame that was actually
    // resolved. Drawing the raw value would show the hitbox a frame late and make
    // this debug view lie about frame data.
    const bool attacking =
        (p.state == ActionState::AttackGround || p.state == ActionState::AttackAir);
    if (!attacking) return;

    if (p.attackId == config::ATK_NONE || p.attackId >= config::ATK_COUNT) return;
    // Read THIS player's table -- frame data is per-character now, so drawing the
    // hitbox from a global table would misreport reach for anyone but Scout.
    const auto &atk = fighterFor(p).attacks[p.attackId];

    const float hx = v.sx(p.x + atk.reachX * p.facing);
    const float hy = v.sy(p.y + atk.reachY);

    // While charging, show the pending hitbox hollow and growing with charge. You
    // can see the threat building before it becomes real.
    if (p.charging) {
        const float t =
            static_cast<float>(p.chargeFrames) / static_cast<float>(config::kSmash.maxChargeFrames);
        const float r = v.len(atk.radius) * (0.6f + 0.4f * t);
        DrawCircleLinesV(Vector2{hx, hy}, r,
                         Color{255, static_cast<unsigned char>(220 - 140 * t), 60, 220});
        return;
    }

    const int resolvedFrame = static_cast<int>(p.attackFrame) - 1;
    const bool live = resolvedFrame >= atk.startup && resolvedFrame < atk.startup + atk.active;
    if (!live) return;

    DrawCircleV(Vector2{hx, hy}, v.len(atk.radius), Color{255, 80, 80, 90});
    DrawCircleLinesV(Vector2{hx, hy}, v.len(atk.radius), Color{255, 60, 60, 220});
}

void drawPlayerHud(const Player &p, int index, int x, int y) {
    const Color labelColor = (index == 0) ? Color{120, 190, 250, 255} : Color{250, 150, 120, 255};

    char line[256];
    std::snprintf(line, sizeof(line), "P%d  %3d%%", index + 1, fx_to_int(p.damage));
    DrawText(line, x, y, 26, labelColor);

    std::snprintf(line, sizeof(line), "%s  f%u", stateName(p.state), p.stateFrame);
    DrawText(line, x, y + 30, 16, Color{200, 200, 210, 255});

    // Velocity is shown DECOMPOSED because that split is architecturally load
    // bearing: selfVel is your own movement, kbVel is knockback, and they decay
    // independently. Seeing both is how you verify air-drifting out of hitstun
    // actually works.
    std::snprintf(line, sizeof(line), "self %+.2f %+.2f", fx_to_float(p.selfVelX),
                  fx_to_float(p.selfVelY));
    DrawText(line, x, y + 50, 16, Color{150, 200, 150, 255});

    std::snprintf(line, sizeof(line), "kb   %+.2f %+.2f", fx_to_float(p.kbVelX),
                  fx_to_float(p.kbVelY));
    DrawText(line, x, y + 68, 16, Color{220, 190, 120, 255});

    int row = y + 90;
    if (p.attackId != config::ATK_NONE) {
        std::snprintf(line, sizeof(line), "> %s", attackName(p.attackId));
        DrawText(line, x, row, 18, Color{255, 210, 140, 255});
        row += 20;
        if (p.charging) {
            std::snprintf(line, sizeof(line), "  CHARGE %d/%d", p.chargeFrames,
                          config::kSmash.maxChargeFrames);
            DrawText(line, x, row, 16, Color{255, 170, 90, 255});
            row += 18;
        }
    }
    if (p.hitstunFrames > 0) {
        std::snprintf(line, sizeof(line), "hitstun %u", p.hitstunFrames);
        DrawText(line, x, row, 16, Color{250, 240, 120, 255});
        row += 18;
    }
    if (p.invulnFrames > 0) {
        std::snprintf(line, sizeof(line), "invuln %u", p.invulnFrames);
        DrawText(line, x, row, 16, Color{180, 220, 255, 255});
        row += 18;
    }
    if (p.lcancelTimer > 0) {
        std::snprintf(line, sizeof(line), "L-cancel window %u", p.lcancelTimer);
        DrawText(line, x, row, 16, Color{200, 160, 255, 255});
        row += 18;
    }
    if (p.fastFalling) {
        DrawText("fast fall", x, row, 16, Color{255, 170, 90, 255});
        row += 18;
    }
    if (p.ledgeSide >= 0) {
        std::snprintf(line, sizeof(line), "on ledge (%s)  hang %u", p.ledgeSide == 0 ? "L" : "R",
                      p.ledgeHangFrames);
        DrawText(line, x, row, 16, Color{120, 200, 255, 255});
        row += 18;
    }
    if (p.ledgeCooldown > 0) {
        std::snprintf(line, sizeof(line), "ledge cooldown %u", p.ledgeCooldown);
        DrawText(line, x, row, 16, Color{200, 120, 120, 255});
        row += 18;
    }
    {
        const fx maxHp = fighterFor(p).shield.maxHealth;
        if (p.shieldInit && p.shieldHealth < maxHp) {
            const float frac = fx_to_float(p.shieldHealth) / fx_to_float(maxHp);
            std::snprintf(line, sizeof(line), "shield %3.0f%%", frac * 100.0f);
            DrawText(line, x, row, 16,
                     frac < 0.25f ? Color{255, 110, 110, 255} : Color{140, 200, 255, 255});
            row += 18;
        }
    }
    if (p.grabPartner != kNoAttacker) {
        std::snprintf(line, sizeof(line), "%s  hold %u",
                      p.isGrabber ? "GRABBING" : "GRABBED (mash!)", p.grabHoldFrames);
        DrawText(line, x, row, 18, Color{255, 170, 255, 255});
        row += 20;
    }
    if (p.shieldStunFrames > 0) {
        std::snprintf(line, sizeof(line), "shieldstun %u", p.shieldStunFrames);
        DrawText(line, x, row, 16, Color{255, 200, 120, 255});
        row += 18;
    }
    if (p.dizzyFrames > 0) {
        std::snprintf(line, sizeof(line), "DIZZY %u (mash!)", p.dizzyFrames);
        DrawText(line, x, row, 18, Color{255, 140, 220, 255});
        row += 20;
    }
    if (p.hitlagFrames > 0) {
        std::snprintf(line, sizeof(line), "HITLAG %u   sdi %u/%d", p.hitlagFrames, p.sdiNudges,
                      config::kSDI.maxNudgesPerHitlag);
        DrawText(line, x, row, 16, Color{255, 240, 140, 255});
        row += 18;
    }
    if (p.techWindow > 0) {
        std::snprintf(line, sizeof(line), "TECH window %u", p.techWindow);
        DrawText(line, x, row, 16, Color{140, 255, 200, 255});
        row += 18;
    }
    if (p.techLockout > 0) {
        std::snprintf(line, sizeof(line), "tech locked %u", p.techLockout);
        DrawText(line, x, row, 16, Color{200, 120, 120, 255});
        row += 18;
    }
    if (p.pendingKOs > 0) { DrawText("KO! (full heal)", x, row, 16, Color{120, 255, 160, 255}); }
}

GameState makeLocalMatch() {
    GameState gs;
    for (int i = 0; i < 2; ++i) {
        Player &p = gs.players[i];
        p = Player{};
        p.active = true;
        // Generous stocks for a tuning session: the point is to keep playing, not
        // to run out of test subjects. The arena sets this from config instead.
        p.stocks = 999;
        p.state = ActionState::Idle;
        p.y = gs.stage.groundY;
        p.facing = (i == 0) ? 1 : -1;
    }
    gs.players[0].x = fxi(480);
    gs.players[1].x = fxi(800);
    return gs;
}

} // namespace

int main() {
    InitWindow(kScreenW, kScreenH, "tier-fighter -- movement playtest");
    SetTargetFPS(kTicksPerSecond);

    GameState gs = makeLocalMatch();
    Input prev[kMaxPlayers] = {};

    const ViewTransform view = fitStage(gs.stage, kScreenW, kScreenH);

    bool showBoxes = true;
    // Stick figures by default; K flips back to boxes. Boxes remain genuinely
    // useful -- they show the hurtbox exactly, which a figure only approximates.
    bool stickFigures = true;
    bool paused = false;
    bool showHelp = true;

    while (!WindowShouldClose()) {
        // --- Debug controls (these act on the harness, never on sim state) ----
        if (IsKeyPressed(KEY_H)) showBoxes = !showBoxes;
        if (IsKeyPressed(KEY_K)) stickFigures = !stickFigures;
        if (IsKeyPressed(KEY_P)) paused = !paused;
        if (IsKeyPressed(KEY_TAB)) showHelp = !showHelp;
        if (IsKeyPressed(KEY_R)) {
            gs = makeLocalMatch();
            std::memset(prev, 0, sizeof(prev));
            // Re-prime the blenders, or the first frame after a reset bridges from
            // whatever pose the players were in before it.
            for (auto &b : gBlenders)
                b = pose::Blender{};
        }

        // Frame-advance while paused. This is how you actually verify frame data:
        // step one frame at a time and watch startup/active windows tick by.
        const bool stepOnce = IsKeyPressed(KEY_N);
        // Slow motion for reading a launch or a wavedash as it happens.
        const bool slowMo = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        const bool advance = (!paused && (!slowMo || (gs.tick % 4 == 0))) || stepOnce;

        Input cur[kMaxPlayers] = {};
        cur[0] = readInput(kP1);
        cur[1] = readInput(kP2);

        if (advance) {
            step(gs, cur, prev);
            std::memcpy(prev, cur, sizeof(cur));
        } else if (slowMo && !paused) {
            // Holding a frame: still advance the tick so the slow-mo gate cycles.
            gs.tick++;
        }

        // --- Draw ------------------------------------------------------------
        BeginDrawing();
        ClearBackground(Color{18, 20, 26, 255});

        drawStage(gs.stage, view);
        for (int i = 0; i < 2; ++i) {
            drawPlayer(gs.players[i], i, view, showBoxes, stickFigures);
        }

        drawPlayerHud(gs.players[0], 0, 24, 20);
        drawPlayerHud(gs.players[1], 1, kScreenW - 240, 20);

        if (paused) {
            DrawText("PAUSED  (N = step one frame)", kScreenW / 2 - 170, 24, 22,
                     Color{255, 220, 120, 255});
        } else if (slowMo) {
            DrawText("SLOW-MO", kScreenW / 2 - 50, 24, 22, Color{160, 220, 255, 255});
        }

        if (showHelp) {
            const char *help =
                "P1: WASD move  SPACE jump  F attack  G shield/airdodge\n"
                "P2: ARROWS move  RSHIFT jump  .  attack  /  shield\n"
                "\n"
                "WAVEDASH: jump, then airdodge down-forward just above the ground.\n"
                "  Horizontal momentum survives the landing -- that is the whole trick.\n"
                "L-CANCEL: press shield just before landing an aerial (halves landing lag).\n"
                "SHORT HOP: release jump during the 4-frame jumpsquat.\n"
                "FAST FALL: hard stick down while already descending.\n"
                "\n"
                "LEDGE: fall past the stage edge to grab. Then toward-stage = climb,\n"
                "  shield = roll, attack = ledge attack, jump = jump, down/away = drop.\n"
                "TECH: press shield just before hitting the ground in hitstun.\n"
                "GRAB: hold G + press F. Then flick a direction to throw,\n"
                "  or press F to pummel. Grabbed? Mash buttons AND stick to escape.\n"
                "SHIELD: hold G. Blocks damage but drains -- run it out and it BREAKS.\n"
                "  Out of shield: flick sideways = roll, flick down = spotdodge.\n"
                "SDI: flick the stick during the hit freeze to shift position.\n"
                "  Re-flick (neutral between) for more -- holding does nothing.\n"
                "SMASH: flick the stick + attack together (hold attack to charge).\n"
                "  Holding a direction first gives a TILT instead.\n"
                "\n"
                "K figures/boxes   H hitboxes   P pause   N step frame   ALT slow-mo   R reset   "
                "TAB hide";
            const int boxH = 260;
            DrawRectangle(16, kScreenH - boxH - 16, 700, boxH, Color{0, 0, 0, 150});
            DrawText(help, 28, kScreenH - boxH - 4, 15, Color{190, 195, 205, 255});
        }

        char footer[128];
        std::snprintf(footer, sizeof(footer), "tick %u   checksum %08x   %d fps", gs.tick,
                      checksum(gs), GetFPS());
        DrawText(footer, kScreenW - 330, kScreenH - 28, 16, Color{110, 115, 125, 255});

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
