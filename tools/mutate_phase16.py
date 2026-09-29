#!/usr/bin/env python3
"""Mutation testing for Phase 16, player behaviour.

Inherits the discipline of `tools/mutate_phase15.py`, and the reason that harness
existed is worth repeating here because this suite has the same traps:

* Every patch is verified to have matched **and** to have survived write-back. A
  patch that matched nothing produces a green run and would be recorded as
  EQUIVALENT, which is the most expensive way for a mutation harness to be wrong.
* Reverts go through `git checkout`, never string replacement. A deletion mutation
  has an empty `new`, and `str.replace("", old)` splices `old` between every character
  of the file. That destroyed a 700-line source file in Phase 15 and reported a pass.
* The post-revert tree must **rebuild and be green** before the next mutation is
  measured. A stale binary makes a clean mutation look like a crash - which is exactly
  what happened to three mutations in Phase 15 before the guard was added.
* A crash is not a detection. ctest reports a segfault as `SEGFAULT`, not `Failed`, and
  the first version of that harness matched only `Failed` and filed every segfault as
  UNDETECTED. Both reasons are captured and bucketed as CRASHED.
* The suite list is read from `ctest -N` and compared, so a harness that quietly ran
  fewer tests than it claims is a hard error rather than a wrong verdict.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-mut")
RESULTS_PATH = os.environ.get(
    "MUTATION_RESULTS",
    os.path.join(tempfile.gettempdir(), "phase16-mutation-results.json"))

INC = "include/engine"
SRC = "src/engine"
LEVEL = "src/engine/level/LevelLoader.cpp"
ASSETS = "assets/assets.txt"

MUTATIONS = []


def mutation(name, description, intent, patches, equivalent=None):
    """`equivalent` is the proven reason a mutation cannot be observed, if it has one.

    A mutation that survives every suite is not automatically a defect in the tests: it
    can be a genuine equivalence. Declaring the reason here means the harness reports
    EQUIVALENT with the explanation attached, rather than leaving it in UNDETECTED where
    it reads as an unexplained failure. Declaring one is a claim, so it has to say why.
    """
    MUTATIONS.append({"name": name, "description": description, "intent": intent,
                      "patches": patches, "equivalent": equivalent})


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


def sysfile(name):
    return f"{SRC}/systems/{name}"


def header(name):
    return f"{INC}/systems/{name}"


# ---------------------------------------------------------------------------
# Movement
# ---------------------------------------------------------------------------

mutation(
    "movement-inverted",
    "Left and right are swapped: pressing A walks right and D walks left.",
    "player_test: 'moving right walks right' and 'moving left walks left'. The two are "
    "separate groups on purpose, because a system that ignored the direction "
    "altogether would satisfy one of them.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        transform.velocity.x = moving ? (movingRight ? config.leftRightSpeed : -config.leftRightSpeed) : 0.0F;",
           "        transform.velocity.x = moving ? (movingLeft ? config.leftRightSpeed : -config.leftRightSpeed) : 0.0F; // MUTATION")],
)

mutation(
    "movement-not-releasing",
    "Releasing the key leaves the horizontal velocity in place, so the player slides on.",
    "player_test: 'releasing movement stops the player'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        transform.velocity.x = moving ? (movingRight ? config.leftRightSpeed : -config.leftRightSpeed) : 0.0F;",
           "        if (moving)\n        {\n"
           "            transform.velocity.x = movingRight ? config.leftRightSpeed : -config.leftRightSpeed;\n"
           "        } // MUTATION: never zeroed")],
)

mutation(
    "walk-speed-hardcoded",
    "The walk speed is a constant instead of the level's `leftRightSpeed`.",
    "player_test: 'the walk speed is the level's, not a constant' - the level in that "
    "group moves at 60, so a hardcoded 200 cannot pass it",
    [patch(sysfile("PlayerSystem.cpp"),
           "        transform.velocity.x = moving ? (movingRight ? config.leftRightSpeed : -config.leftRightSpeed) : 0.0F;",
           "        transform.velocity.x = moving ? (movingRight ? 200.0F : -200.0F) : 0.0F; // MUTATION")],
)

mutation(
    "max-speed-clamp-removed",
    "The maximum speed is not clamped at all.",
    "player_test: 'the maximum speed clamps in both directions' - a level with "
    "movement 900 and maximum 50 cannot pass without the clamp",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (transform.velocity.x > config.maxSpeed)",
           "        if (false) // MUTATION: no upper clamp"),
     patch(sysfile("PlayerSystem.cpp"),
           "        else if (transform.velocity.x < -config.maxSpeed)",
           "        else if (false) // MUTATION: no lower clamp")],
)

mutation(
    "max-speed-clamp-one-sided",
    "Only the upper clamp survives; the lower one is removed, so the player can move "
    "left faster than the maximum.",
    "player_test: 'the maximum speed clamps in both directions'. Deliberately "
    "one-sided: the two branches are separate code and a sign error in one of them "
    "leaves the other correct.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        else if (transform.velocity.x < -config.maxSpeed)",
           "        else if (false) // MUTATION: lower clamp removed")],
)

mutation(
    "max-speed-hardcoded",
    "The clamp uses a constant rather than the level's `maxSpeed`.",
    "player_test: 'the maximum speed clamps in both directions' - a maximum of 50 "
    "cannot be produced by a hardcoded 250",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (transform.velocity.x > config.maxSpeed)",
           "        if (transform.velocity.x > 250.0F) // MUTATION"),
     patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.x = config.maxSpeed;",
           "            transform.velocity.x = 250.0F; // MUTATION"),
     patch(sysfile("PlayerSystem.cpp"),
           "        else if (transform.velocity.x < -config.maxSpeed)",
           "        else if (transform.velocity.x < -250.0F) // MUTATION"),
     patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.x = -config.maxSpeed;",
           "            transform.velocity.x = -250.0F; // MUTATION")],
)

mutation(
    "vertical-movement-from-moveup",
    "The player walks upwards on `MoveUp`, which shares the W key with the jump.",
    "player_test: 'the jump key launches the player at the level's jump speed' and "
    "'holding the jump key jumps only once'. A player that rises while W is held "
    "climbs forever, which is double-jumping wearing a different hat.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        const bool movingLeft = actions.isActive(Action::MoveLeft);",
           "        const bool movingUp = actions.isActive(Action::MoveUp); // MUTATION\n"
           "        static_cast<void>(movingUp);\n"
           "        const bool movingLeft = actions.isActive(Action::MoveLeft);"),
     patch(sysfile("PlayerSystem.cpp"),
           "        if (!player.grounded)\n        {\n            transform.velocity.y += config.gravity * deltaSeconds;",
           "        if (!player.grounded || actions.isActive(Action::MoveUp)) // MUTATION\n"
           "        {\n            transform.velocity.y += config.gravity * deltaSeconds;\n"
           "            if (actions.isActive(Action::MoveUp))\n            {\n"
           "                transform.velocity.y -= config.leftRightSpeed;\n            }")],
)

# ---------------------------------------------------------------------------
# Facing
# ---------------------------------------------------------------------------

mutation(
    "facing-inverted",
    "Facing is set to -1 for right and +1 for left.",
    "player_test: 'facing follows the direction pressed'",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.scale.x = movingRight ? 1.0F : -1.0F;",
           "            transform.scale.x = movingRight ? -1.0F : 1.0F; // MUTATION")],
)

mutation(
    "facing-always-right",
    "Facing is forced to +1 on every frame that a direction is held, so a player "
    "walking left is drawn facing right.",
    "player_test: 'facing follows the direction pressed'",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.scale.x = movingRight ? 1.0F : -1.0F;",
           "            transform.scale.x = 1.0F; // MUTATION: always right")],
)

mutation(
    "facing-reset-when-stopped",
    "Facing is written unconditionally each frame, so releasing the key resets it to "
    "right.",
    "player_test: 'facing survives stopping, jumping and landing'. The course says "
    "facing lasts 'until the other direction has been pressed', so a reset on release "
    "is the specific bug.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (moving)\n        {\n            transform.scale.x = movingRight ? 1.0F : -1.0F;\n        }",
           "        transform.scale.x = movingRight ? 1.0F : (movingLeft ? -1.0F : 1.0F); // MUTATION")],
)

mutation(
    "facing-whole-scale-on-jump",
    "Jumping writes the whole `scale`, zeroing x, which renders as no sprite at all.",
    "player_test: 'jumping does not reset facing' - which also asserts that the "
    "magnitude stays exactly 1",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y = -config.jumpSpeed;",
           "            transform.velocity.y = -config.jumpSpeed;\n"
           "            transform.scale = Vec2{0.0F, 1.0F}; // MUTATION")],
)

# ---------------------------------------------------------------------------
# Jump
# ---------------------------------------------------------------------------

mutation(
    "jump-level-triggered",
    "The jump uses `isActive` instead of `wasPressed`, so holding W re-launches the "
    "player every frame.",
    "player_test: 'holding the jump key jumps only once' (the peak is one launch's "
    "worth and the player lands again) and 'the jump key launches the player at the "
    "level's jump speed'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (actions.wasPressed(Action::Jump) && player.grounded)",
           "        if (actions.isActive(Action::Jump) && player.grounded) // MUTATION")],
)

mutation(
    "jump-in-the-air",
    "The grounded requirement is dropped, so an airborne player can jump again.",
    "player_test: 'the jump only works from the ground' (three fresh presses mid-air) "
    "and 'an airborne player is in Air even while moving horizontally'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (actions.wasPressed(Action::Jump) && player.grounded)",
           "        if (actions.wasPressed(Action::Jump)) // MUTATION: no grounded check")],
)

mutation(
    "jump-positive",
    "The jump sets a *downward* velocity, so `W` makes the player fall faster.",
    "player_test: 'the jump key launches the player at the level's jump speed' and "
    "'releasing the jump key stops the ascent immediately'",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y = -config.jumpSpeed;",
           "            transform.velocity.y = config.jumpSpeed; // MUTATION: downwards")],
)

mutation(
    "jump-speed-hardcoded",
    "The jump speed is a constant rather than the level's.",
    "player_test: 'the jump key launches the player at the level's jump speed' and "
    "'a short press is a shorter jump than a held key', whose peak is tied to the "
    "level's own 400",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y = -config.jumpSpeed;",
           "            transform.velocity.y = -400.0F; // MUTATION")],
)

# ---------------------------------------------------------------------------
# Variable jump
# ---------------------------------------------------------------------------

mutation(
    "variable-jump-removed",
    "Releasing the key no longer stops the ascent, so every jump is full height.",
    "player_test: 'a short press is a shorter jump than a held key' - the tap and the "
    "held jump would reach the same height",
    [patch(sysfile("PlayerSystem.cpp"),
           "        else if (player.jumping && !actions.isActive(Action::Jump) && transform.velocity.y < 0.0F)",
           "        else if (false) // MUTATION: no variable jump")],
)

mutation(
    "variable-jump-halves",
    "Releasing the key halves the upward velocity instead of stopping it.",
    "player_test: 'a short press is a shorter jump than a held key'. A plausible "
    "alternative design, and the reason the group's assertion is a *ratio* rather than "
    "a specific height.",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y = 0.0F;\n            player.jumping = false;",
           "            transform.velocity.y *= 0.5F; // MUTATION: halve, not stop\n"
           "            player.jumping = false;")],
)

mutation(
    "variable-jump-cancels-the-fall",
    "The `velocity.y < 0` guard is removed, so releasing the key while falling zeroes "
    "the descent every frame and the player hovers.",
    "player_test: 'releasing the jump key while falling does not cancel the fall'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        else if (player.jumping && !actions.isActive(Action::Jump) && transform.velocity.y < 0.0F)",
           "        else if (player.jumping && !actions.isActive(Action::Jump)) // MUTATION: no guard")],
)

# ---------------------------------------------------------------------------
# The state machine
# ---------------------------------------------------------------------------

mutation(
    "state-never-air",
    "`Air` is never selected, so a jumping player is Stand or Run the whole time.",
    "player_test: 'the jump frame is airborne immediately', 'an airborne player is in "
    "Air even while moving horizontally', 'landing returns to a grounded state' and "
    "'walking off a ledge makes the player airborne'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        player.state = airborne ? PlayerState::Air : (moving ? PlayerState::Run : PlayerState::Stand);",
           "        player.state = moving ? PlayerState::Run : PlayerState::Stand; // MUTATION: never Air")],
)

mutation(
    "state-air-ignores-jumping",
    "The state uses `grounded` alone, so the jump frame reads Stand - a one-frame "
    "stutter at the bottom of every jump.",
    "player_test: 'the jump frame is airborne immediately' asserts Air on the frame "
    "the jump starts, while `grounded` is still true",
    [patch(sysfile("PlayerSystem.cpp"),
           "        const bool airborne = !player.grounded || player.jumping;",
           "        const bool airborne = !player.grounded; // MUTATION: jumping ignored")],
)

mutation(
    "state-run-ignores-standing",
    "Any frame at all is Run, including a player standing perfectly still.",
    "player_test: 'a still grounded player is standing' and 'a moving grounded player "
    "is running' (which checks the return to Stand on release)",
    [patch(sysfile("PlayerSystem.cpp"),
           "        player.state = airborne ? PlayerState::Air : (moving ? PlayerState::Run : PlayerState::Stand);",
           "        player.state = airborne ? PlayerState::Air : PlayerState::Run; // MUTATION: never Stand")],
)

mutation(
    "state-air-beats-nothing",
    "The state is Stand whenever grounded, even while moving - so Run is unreachable.",
    "player_test: 'a moving grounded player is running'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        player.state = airborne ? PlayerState::Air : (moving ? PlayerState::Run : PlayerState::Stand);",
           "        player.state = airborne ? PlayerState::Air : PlayerState::Stand; // MUTATION: never Run")],
)

# ---------------------------------------------------------------------------
# Gravity
# ---------------------------------------------------------------------------

mutation(
    "gravity-removed",
    "Gravity is never applied, so a player who walks off a ledge floats.",
    "player_test: 'gravity accelerates the player downward', 'variable jump does not "
    "change the global physics behaviour' and 'an airborne player is in Air even while "
    "moving horizontally'",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y += config.gravity * deltaSeconds;",
           "            // MUTATION: no gravity")],
)

mutation(
    "gravity-applied-while-grounded",
    "Gravity is applied every frame, including while standing, so a resting player "
    "accumulates a downward velocity that the collision cancels every frame.",
    "player_test: 'a grounded player carries no vertical speed' and 'landing clears "
    "the downward velocity'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (!player.grounded)\n        {\n            transform.velocity.y += config.gravity * deltaSeconds;",
           "        if (true) // MUTATION: always\n        {\n            transform.velocity.y += config.gravity * deltaSeconds;")],
)

mutation(
    "gravity-hardcoded",
    "Gravity is a constant rather than the level's.",
    "player_test: 'gravity accelerates the player downward' checks the velocity after "
    "exactly one and two frames, against 900 px/s",
    [patch(sysfile("PlayerSystem.cpp"),
           "            transform.velocity.y += config.gravity * deltaSeconds;",
           "            transform.velocity.y += 500.0F * deltaSeconds; // MUTATION")],
)

# ---------------------------------------------------------------------------
# The ground probe
# ---------------------------------------------------------------------------

mutation(
    "grounded-always-true",
    "The ground probe always reports grounded, so a player never falls and can jump "
    "in mid-air.",
    "player_test: 'walking off a ledge makes the player airborne', 'landing returns to "
    "a grounded state' and 'the jump only works from the ground'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        player.grounded = isGroundedOn(entities, transform.position, collider.size, entity);",
           "        player.grounded = true; // MUTATION: always grounded")],
)

mutation(
    "grounded-always-false",
    "The ground probe always reports airborne, so a standing player is in Air and can "
    "never jump.",
    "player_test: 'a still grounded player is standing' and 'the jump key launches the "
    "player at the level's jump speed'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        player.grounded = isGroundedOn(entities, transform.position, collider.size, entity);",
           "        player.grounded = false; // MUTATION: never grounded")],
)

mutation(
    "grounded-probe-one-sided",
    "The probe's vertical test is one-sided again, so a player well above the floor "
    "counts as standing on it.",
    "player_test: 'walking off a ledge makes the player airborne' and 'the jump only "
    "works from the ground'. The two-sided test is subtle enough to be worth "
    "re-breaking deliberately: the first version of this function had it one-sided and "
    "it looked correct.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (rise <= kGroundProbeDepth && rise >= -kGroundProbeDepth)",
           "        if (rise >= -kGroundProbeDepth) // MUTATION: one-sided")],
)

mutation(
    "grounded-probe-ignores-horizontal-overlap",
    "The probe ignores horizontal overlap, so a player standing in mid-air beside a "
    "column of tiles is grounded.",
    "player_test: 'walking off a ledge makes the player airborne' - the floor ends and "
    "the player walks past it with the tiles still overlapping on x",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (player.overlapX(ground) <= 0.0F)\n        {\n            continue;\n        }",
           "        // MUTATION: no horizontal check")],
)

mutation(
    "grounded-probe-ignores-body-type",
    "The probe treats every collider as ground, so the player's own box can be its own "
    "floor.",
    "player_test: 'landing returns to a grounded state' and 'a still grounded player is "
    "standing' - with the player counted as ground it never reports airborne",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (!isSolidGround(body))\n        {\n            continue;\n        }",
           "        // MUTATION: every body is ground")],
)

# ---------------------------------------------------------------------------
# Respawn
# ---------------------------------------------------------------------------

mutation(
    "respawn-disabled",
    "Falling out of the world does not respawn the player.",
    "player_test: 'falling out of the world respawns the player'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (hasFallenOut(transform.position, collider.size, m_fallLimitY))",
           "        if (false) // MUTATION: never respawn")],
)

mutation(
    "respawn-magic-number",
    "The fall limit is a hardcoded 10000 rather than the world's bottom edge.",
    "player_test: 'the fall limit is the world's bottom edge' reads the number off a "
    "real PlayScene and compares it with `cellsTall * kCellSize`. This is the exact "
    "value the phase brief says not to use.",
    [patch(sysfile("PlayerSystem.cpp"),
           "        if (hasFallenOut(transform.position, collider.size, m_fallLimitY))",
           "        if (hasFallenOut(transform.position, collider.size, 10000.0F)) // MUTATION")],
)

mutation(
    "respawn-position-only",
    "The respawn restores the position but not the velocity.",
    "player_test: 'falling out of the world respawns the player' checks both velocity "
    "axes, and cannot fail - because the respawn runs at step 2 and steps 3 and 6 zero "
    "the same two components on the same frame. Step 3 writes `velocity.x` from the "
    "input and step 6 writes `velocity.y` for the now-grounded player, so the deleted "
    "line has no observable effect. It is kept anyway, and the reasoning is written "
    "next to it in PlayerSystem.cpp: a respawn that reads as 'restore everything' should "
    "not depend on two later steps happening to agree.",
    [patch(sysfile("PlayerSystem.cpp"),
           "    transform.velocity = Vec2{0.0F, 0.0F};\n    transform.scale = Vec2{1.0F, 1.0F};",
           "    transform.scale = Vec2{1.0F, 1.0F}; // MUTATION: velocity kept")],
    equivalent="A respawn at step 2 is immediately followed by step 3 writing "
               "velocity.x from the input and step 6 writing velocity.y for a grounded "
               "player, so both components are zeroed on the same frame whether or not "
               "the respawn zeroes them. Proven equivalent, and kept deliberately.",
)

mutation(
    "respawn-keeps-jump-state",
    "The respawn does not clear the `jumping` flag, so the player is in Air and falls "
    "straight through the world again.",
    "player_test: 'no stale jump state survives a respawn' and 'a respawned player can "
    "jump immediately'",
    [patch(sysfile("PlayerSystem.cpp"),
           "    player.grounded = true;\n    player.jumping = false;\n    player.state = PlayerState::Stand;",
           "    player.grounded = true;\n    player.state = PlayerState::Stand; // MUTATION: jumping kept")],
)

mutation(
    "respawn-keeps-facing",
    "The respawn does not reset facing, so a player who fell while walking left comes "
    "back facing left.",
    "player_test: 'falling out of the world respawns the player', which sets the scale "
    "to -1 and requires +1 afterwards",
    [patch(sysfile("PlayerSystem.cpp"),
           "    transform.scale = Vec2{1.0F, 1.0F};",
           "    // MUTATION: facing kept")],
)

mutation(
    "respawn-uses-a-magic-position",
    "The respawn puts the player at a fixed (0, 0) rather than at the level's spawn.",
    "player_test: 'falling out of the world respawns the player' - the player spawns at "
    "x=20, so a fixed origin is not the same point",
    [patch(sysfile("PlayerSystem.cpp"),
           "    transform.position = player.spawnPosition;",
           "    transform.position = Vec2{0.0F, 0.0F}; // MUTATION")],
)

# ---------------------------------------------------------------------------
# Animation selection
# ---------------------------------------------------------------------------

mutation(
    "animation-always-stand",
    "Every state selects the standing animation, so the player never draws airborne.",
    "player_test: 'each state selects its own animation' and 'the animation changes "
    "when the state changes'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        case PlayerState::Air:\n            return kAirAnimation;",
           "        case PlayerState::Air:\n            return kStandAnimation; // MUTATION")],
)

mutation(
    "animation-always-air",
    "Every state selects the airborne animation, so a grounded player looks like it is "
    "jumping.",
    "player_test: 'each state selects its own animation'",
    [patch(sysfile("PlayerSystem.cpp"),
           "        case PlayerState::Stand:\n            return kStandAnimation;",
           "        case PlayerState::Stand:\n            return kAirAnimation; // MUTATION")],
)

mutation(
    "animation-run-name-invented",
    "The run animation is given a name the asset table does not declare, which throws "
    "out of the animation system the first time a player runs.",
    "player_test: 'each state selects its own animation' - and the game's own frames "
    "would throw, which is what makes a name a contract rather than a label",
    [patch(sysfile("PlayerSystem.cpp"),
           "constexpr std::string_view kRunAnimation = kStandAnimation;",
           "constexpr std::string_view kRunAnimation = \"megaman_megaRun_run\"; // MUTATION")],
)

mutation(
    "animation-ended-carries-over",
    "`selectAnimation` keeps `ended` when the name changes, so a player whose previous "
    "animation had finished is destroyed by the animation system on the same frame.",
    "player_test: 'the animation frame resets when the animation changes', which leaves "
    "`ended` set on the entity and requires the player to still be in the world",
    [patch(sysfile("PlayerSystem.cpp"),
           "    animation.ticksOnFrame = 0U;\n    animation.repeat = true;\n    animation.ended = false;",
           "    animation.ticksOnFrame = 0U;\n    animation.repeat = true;\n    // MUTATION: ended not reset")],
)

mutation(
    "animation-not-repeating",
    "The player's animation is set not to repeat, so it reports itself ended and the "
    "animation system deletes the player.",
    "player_test: 'the player animation loops so it is never destroyed' and 'each "
    "state selects its own animation'",
    [patch(sysfile("PlayerSystem.cpp"),
           "    animation.repeat = true;\n    animation.ended = false;",
           "    animation.repeat = false; // MUTATION\n    animation.ended = false;")],
)

# ---------------------------------------------------------------------------
# The spawn, and the level data it comes from
# ---------------------------------------------------------------------------

mutation(
    "spawn-without-player-state",
    "The loader spawns the player without a `components::Player`, so the behaviour "
    "system silently skips it and the player never moves.",
    "player_test: 'the committed level spawns a player with those numbers' (which "
    "checks the component exists) and every movement group. A missing component makes "
    "the query yield nothing, so the failure is a world where nothing happens at all.",
    [patch(LEVEL,
           "            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, false, false,\n"
           "                                                                        position});",
           "            // MUTATION: no Player component")],
)

mutation(
    "spawn-without-animation",
    "The loader spawns the player without an animation, so it is not drawn.",
    "player_test: 'the committed level spawns a player with those numbers' and "
    "'the player is drawn as an animation with its sprite centred'",
    [patch(LEVEL,
           "            entity.addComponent<components::Animation>(loopingAnimation(std::string{kPlayerStandAnimation}));",
           "            // MUTATION: no animation")],
)

mutation(
    "spawn-without-collider",
    "The loader spawns the player without a collider, so it falls through the floor.",
    "player_test: 'the player is spawned from level data' and every gravity group",
    [patch(LEVEL,
           "            entity.addComponent<components::Collider>(components::Collider{size});\n"
           "            entity.addComponent<components::Body>(components::Body{physics::BodyType::Dynamic});",
           "            entity.addComponent<components::Body>(components::Body{physics::BodyType::Dynamic}); // MUTATION: no collider")],
)

mutation(
    "spawn-collider-hardcoded",
    "The player's collider is a fixed 32x32 instead of the level's 40x60.",
    "player_test: 'the player is spawned from level data' checks the collider against "
    "the level's 40x60, and 'the sprite and the collider share the transform position' "
    "compares the sprite frame against it",
    [patch(LEVEL,
           "            const Vec2 size = player.boundingBoxSize;",
           "            const Vec2 size{32.0F, 32.0F}; // MUTATION: not the level's")],
)

mutation(
    "spawn-config-zeroed",
    "The player's level constants are zeroed, so it cannot move, jump or fall.",
    "player_test: 'the player is spawned from level data' checks the four numbers",
    [patch(LEVEL,
           "                components::PlayerConfig{player.leftRightSpeed, player.jumpSpeed, player.maxSpeed, player.gravity,\n"
           "                                         player.bulletAnimationName});",
           "                components::PlayerConfig{0.0F, 0.0F, 0.0F, 0.0F, player.bulletAnimationName}); // MUTATION")],
)

mutation(
    "spawn-anchor-wrong",
    "The respawn anchor is a different point from the spawn position, so a respawn puts "
    "the player somewhere the level never asked for.",
    "player_test: 'the spawn position comes from the grid cell' compares the two, and "
    "'falling out of the world respawns the player' restores the anchor",
    [patch(LEVEL,
           "            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, false, false,\n"
           "                                                                        position});",
           "            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, false, false,\n"
           "                                                                        Vec2{0.0F, 0.0F}}); // MUTATION")],
)

mutation(
    "player-declared-grounded",
    "The loader reports the freshly spawned player as grounded, so it can jump on its "
    "first frame from the middle of the air.",
    "player_test: 'the player is spawned from level data' indirectly - the committed "
    "level spawns the player at cell (3, 4), in the air, and the fall is what the "
    "gravity groups measure",
    [patch(LEVEL,
           "            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, false, false,\n"
           "                                                                        position});",
           "            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, true, false,\n"
           "                                                                        position}); // MUTATION")],
)

mutation(
    "player-also-a-rectangle",
    "The player gets a rectangle alongside its animation, so the renderer's two "
    "separate queries draw it twice.",
    "level_test: 'a tile has no rectangle so it is not drawn twice' counts rectangles "
    "and requires zero",
    [patch(LEVEL,
           "            entity.addComponent<components::Animation>(loopingAnimation(std::string{kPlayerStandAnimation}));",
           "            entity.addComponent<components::Animation>(loopingAnimation(std::string{kPlayerStandAnimation}));\n"
           "            entity.addComponent<components::Rectangle>(components::Rectangle{size, engine::kWhite}); // MUTATION"),
     patch(LEVEL, "#include \"engine/components/Player.hpp\"",
           "#include \"engine/components/Player.hpp\"\n#include \"engine/components/Rectangle.hpp\"\n#include \"engine/Color.hpp\"")],
)

# ---------------------------------------------------------------------------
# The assets, and the run artwork that cannot be loaded
# ---------------------------------------------------------------------------

mutation(
    "run-animation-declared-anyway",
    "The 733-pixel run strip is declared as the course's 3 frames, which does not "
    "divide.",
    "asset_loader_test: 'declaring the run strip as three frames is rejected' is "
    "written against a configuration in memory, so it passes either way - the real "
    "detection is that the *shipped* configuration now fails to load, which stops "
    "every suite that builds an asset manager.",
    [patch(ASSETS, "Animation megaman_megaStand_stand  megaman_megaStand  1  1",
           "Animation megaman_megaStand_stand  megaman_megaStand  1  1\n"
           "Animation megaman_megaRun_run      megaman_megaRun    3  8  # MUTATION")],
)

mutation(
    "air-animation-removed",
    "The airborne player animation is removed from the configuration, so a jumping "
    "player throws the moment it draws.",
    "player_test: 'each state selects its own animation' and 'every animation the "
    "player can select is a declared asset'",
    [patch(ASSETS, "Animation megaman_megaJump_air     megaman_megaJump    1  1\n", "")],
)

# ---------------------------------------------------------------------------
# The update order
# ---------------------------------------------------------------------------

mutation(
    "player-system-after-physics",
    "`PlayerSystem` is registered after `PhysicsSystem`, so the player's velocity is "
    "integrated on the following frame.",
    "player_test: 'player behaviour lands in the same frame as the input' moves the "
    "player by exactly `speed * dt` in one frame, and scene_test: 'the play scene "
    "registers its own systems' pins the order by name",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n"
           "    m_systems.add<engine::systems::PhysicsSystem>();",
           "    m_systems.add<engine::systems::PhysicsSystem>();\n"
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize); // MUTATION")],
)

mutation(
    "player-system-missing",
    "`PlayerSystem` is not registered at all, so the player is an inert sprite.",
    "scene_test: 'the play scene registers its own systems' (the count) and every "
    "player_test movement group",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n", "")],
)

mutation(
    "fall-limit-wrong-value",
    "`PlayScene` passes a fall limit that is not the world's bottom edge.",
    "player_test: 'the fall limit is the world's bottom edge' reads the number off a "
    "real PlayScene and compares it with `cellsTall * kCellSize`",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "m_cellsTall * level::LevelGrid::kCellSize);",
           "m_cellsTall * level::LevelGrid::kCellSize * 4.0F); // MUTATION")],
)

# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------


def read(path):
    with open(os.path.join(ROOT, path), "r", encoding="utf-8") as handle:
        return handle.read()


def write(path, text):
    with open(os.path.join(ROOT, path), "w", encoding="utf-8") as handle:
        handle.write(text)


def apply_patches(entry):
    for spec in entry["patches"]:
        original = read(spec["path"])
        found = original.count(spec["old"])
        if found != spec["count"]:
            raise RuntimeError("patch target found {} time(s) in {}, expected {}: {!r}".format(
                found, spec["path"], spec["count"], spec["old"][:70]))
        write(spec["path"], original.replace(spec["old"], spec["new"]))


def revert(entry):
    """Restores every touched file with git, not with string replacement."""
    touched = sorted({spec["path"] for spec in entry["patches"]})
    subprocess.run(["git", "checkout", "--"] + touched, cwd=ROOT, check=True)
    dirty = subprocess.run(["git", "status", "--porcelain", "--"] + touched,
                           cwd=ROOT, capture_output=True, text=True).stdout.strip()
    if dirty:
        raise RuntimeError("revert left these files modified: {}".format(dirty))


def build():
    result = subprocess.run(["cmake", "--build", BUILD, "-j", "8"],
                            cwd=ROOT, capture_output=True, text=True)
    return result.returncode == 0, (result.stdout + result.stderr)


def run_all_tests(expected):
    result = subprocess.run(["ctest", "--output-on-failure", "--timeout", "300"],
                            cwd=BUILD, capture_output=True, text=True)
    output = result.stdout + result.stderr

    ran = re.findall(r"^\s*\d+/\d+ Test\s+#\d+:\s+(\S+)", output, re.MULTILINE)

    crashed = {}
    for name, reason in re.findall(r"^\s*\d+ - (\S+) \((\S+(?: \S+)*)\)", output, re.MULTILINE):
        if reason != "Passed":
            crashed[name] = reason

    timeouts = bool(re.search(r"\*\*\*Timeout|\bTimeout\b", output))
    clean_failed = [name for name, reason in crashed.items() if reason == "Failed"]
    abnormal = {name: reason for name, reason in crashed.items() if reason != "Failed"}

    if sorted(ran) != sorted(expected):
        raise RuntimeError("harness ran a different set of suites.\n  ran:     {}\n  expected: {}".format(
            sorted(ran), sorted(expected)))

    return clean_failed, abnormal, timeouts, output


def run_game():
    binary = os.path.join(BUILD, "game")
    if not os.path.exists(binary):
        return None, "no game binary"
    result = subprocess.run([binary, "--frames", "5"], cwd=ROOT, capture_output=True,
                            text=True, timeout=300)
    return result.returncode, (result.stdout + result.stderr)


def failing_checks(output):
    found = {}
    for match in re.finditer(r"^\s*(\S+\.cpp):(\d+): CHECK\((.*?)\) failed", output,
                             re.MULTILINE):
        found.setdefault(os.path.basename(match.group(1)), []).append(
            "{}:{}".format(match.group(2), match.group(3)))
    return found


def classify(clean_failed, abnormal, ran, build_ok, timeouts, equivalent):
    """The five outcomes, decided by what the run actually did.

    ### A clean failure outranks a crash *somewhere else*

    `megaman_megaRun` declared as 3 frames makes the asset manager throw from its
    constructor, so every suite that builds one aborts - and eight suites also fail with
    real assertion failures first. Reporting that as CRASHED alone would throw away eight
    genuine detections and make the mutation look like a defect in the tests. Reporting
    it as TEST_DETECTED alone would hide the crashes.

    So a clean `Failed` is a detection, and the abnormal suites are recorded alongside
    it in the detail. CRASHED is reserved for the case where **no** suite managed to
    assert anything, which is the case where the crash is all there is to report.
    """
    if not build_ok:
        return "BUILD_REJECTED"
    if clean_failed:
        return "TEST_DETECTED"
    if timeouts or abnormal:
        return "CRASHED"
    if equivalent:
        return "EQUIVALENT"
    return "UNDETECTED"


def verify_green(expected, label):
    clean, abnormal, timeouts, _ = run_all_tests(expected)
    if clean or abnormal or timeouts:
        raise RuntimeError("{} is not green: clean={} abnormal={} timeouts={}".format(
            label, clean, abnormal, timeouts))


def main():
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    entries = [m for m in MUTATIONS if not only or m["name"] in only]

    shutil.rmtree(BUILD, ignore_errors=True)
    configure = subprocess.run(
        ["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Debug"],
        capture_output=True, text=True)
    if configure.returncode != 0:
        print(configure.stdout + configure.stderr)
        raise SystemExit("configure failed")

    ok, output = build()
    if not ok:
        print(output[-4000:])
        raise SystemExit("baseline build failed")

    listing = subprocess.run(["ctest", "-N"], cwd=BUILD, capture_output=True, text=True)
    expected = re.findall(r"Test\s+#\d+:\s+(\S+)", listing.stdout)

    verify_green(expected, "the baseline")
    print("baseline: {} of {} suites green; the game binary runs per mutation too".format(
        len(expected), len(expected)), flush=True)

    results = []
    for entry in entries:
        started = time.time()
        print("=== {} ===".format(entry["name"]), flush=True)
        try:
            apply_patches(entry)
            for spec in entry["patches"]:
                if spec["new"] and spec["new"] not in read(spec["path"]):
                    raise RuntimeError("patch did not survive write-back: {}".format(spec["path"]))

            built, build_output = build()
            if not built:
                outcome = "BUILD_REJECTED"
                lines = build_output.strip().splitlines()
                detail = lines[-1] if lines else ""
                clean_failed, abnormal = [], {}
            else:
                clean_failed, abnormal, timeouts, test_output = run_all_tests(expected)
                game_code, _ = run_game()
                if game_code is not None and game_code != 0:
                    abnormal["game"] = "exit {}".format(game_code)
                outcome = classify(clean_failed, abnormal, expected, built, timeouts,
                                   entry.get("equivalent"))
                detail = json.dumps({"clean_failed": clean_failed, "abnormal": abnormal,
                                     "game_exit": game_code, "equivalent": entry.get("equivalent")})
                checks = failing_checks(test_output)
                detail += "\n    " + json.dumps(checks)[:900]
        except Exception as error:  # noqa: BLE001
            outcome = "HARNESS_ERROR"
            detail = str(error)
        finally:
            revert(entry)

        restored, restored_output = build()
        if not restored:
            raise RuntimeError("rebuild after reverting {} failed:\n{}".format(
                entry["name"], restored_output[-2000:]))
        verify_green(expected, "the tree after reverting " + entry["name"])

        results.append({"name": entry["name"], "description": entry["description"],
                        "intent": entry["intent"], "outcome": outcome,
                        "clean_failed": clean_failed, "abnormal": abnormal,
                        "detail": detail, "seconds": round(time.time() - started, 1)})
        print("    -> {} ({:.0f}s)".format(outcome, time.time() - started), flush=True)

    with open(RESULTS_PATH, "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=2)

    print("\n===== SUMMARY =====")
    tally = {}
    for result in results:
        tally.setdefault(result["outcome"], []).append(result["name"])
    for outcome in sorted(tally):
        print("{:16} {}".format(outcome, len(tally[outcome])))
        for name in tally[outcome]:
            reason = next((m["equivalent"] for m in MUTATIONS if m["name"] == name), None)
            print("    - {}{}".format(name, "  [proven equivalent]" if reason else ""))
    print("total: {} mutations".format(len(results)))


if __name__ == "__main__":
    main()
