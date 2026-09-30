#!/usr/bin/env python3
"""Mutation testing for Phase 17, collision refinement.

Inherits the discipline of `tools/mutate_phase16.py`, and every item in that header
still applies. The Phase 17-specific traps were:

* A patch that matches nothing produces a green run and would be recorded as
  EQUIVALENT, which is the most expensive way for a mutation harness to be wrong. Every
  patch is verified to have matched the expected number of times *and* to have
  survived write-back.
* Reverts go through `git checkout`, never string replacement: a deletion mutation has
  an empty `new`, and `str.replace("", old)` splices `old` between every character.
* The post-revert tree must rebuild and be green before the next mutation, or a clean
  mutation is judged against a stale binary.
* A crash is not a detection. ctest reports a segfault as `SEGFAULT`, not `Failed`, and
  a harness that matches only `Failed` files every segfault as UNDETECTED. Both reasons
  are captured and bucketed as CRASHED - but a clean `Failed` outranks a crash
  *elsewhere*, so a mutation that fails eight suites and aborts a ninth is still
  TEST_DETECTED with the abort recorded beside it.
* The suite list is read from `ctest -N` and compared.

Phase 17 adds one more: a mutation can be a **proven** equivalence, and the harness is
given a place to say why. Leaving a proven equivalence in UNDETECTED makes it
indistinguishable from an unexplained miss.
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
    os.path.join(tempfile.gettempdir(), "phase17-mutation-results.json"))

INC = "include/engine"
SRC = "src/engine"

AABB = f"{INC}/physics/Aabb.hpp"
COLLISION_H = f"{INC}/physics/Collision.hpp"
COLLISION_C = f"{SRC}/physics/Collision.cpp"
PHYSICS_H = f"{SRC}/systems/PhysicsSystem.hpp"
PHYSICS_C = f"{SRC}/systems/PhysicsSystem.cpp"
PLAYER_C = f"{SRC}/systems/PlayerSystem.cpp"
STATE_C = f"{SRC}/systems/PlayerStateSystem.cpp"
SCENE_C = f"{SRC}/scene/PlayScene.cpp"
TRANSFORM = f"{INC}/components/Transform.hpp"

MUTATIONS = []


def mutation(name, description, intent, patches, equivalent=None):
    MUTATIONS.append({"name": name, "description": description, "intent": intent,
                      "patches": patches, "equivalent": equivalent})


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


# ---------------------------------------------------------------------------
# getOverlap: the overlap arithmetic
# ---------------------------------------------------------------------------

mutation(
    "overlap-x-inverted",
    "The x penetration is computed with the centres added rather than subtracted.",
    "collision_test: every group that checks an x overlap, and the reversed-order group "
    "which would see one side disagree with the other.",
    [patch(AABB,
           "        return Vec2{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                    (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)};",
           "        return Vec2{distanceX(other) - (m_halfExtents.x + other.m_halfExtents.x),\n"
           "                    (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)}; // MUTATION")],
)

mutation(
    "overlap-y-inverted",
    "The y penetration is computed with the centres added rather than subtracted.",
    "collision_test: the landing, ceiling and side cases, which are distinguished purely "
    "by the sign of the y penetration.",
    [patch(AABB,
           "        return Vec2{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                    (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)};",
           "        return Vec2{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                    distanceY(other) - (m_halfExtents.y + other.m_halfExtents.y)}; // MUTATION")],
)

mutation(
    "overlap-clamped-at-zero",
    "The penetration is clamped at zero, so separated and touching become the same "
    "number.",
    "collision_test: 'separated boxes give the negative gap' (which needs -80) and the "
    "whole of `landedOn`, whose clauses compare the previous overlap against zero.",
    [patch(AABB,
           "        return Vec2{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                    (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)};",
           "        const Vec2 raw{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                       (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)};\n"
           "        return Vec2{raw.x < 0.0F ? 0.0F : raw.x, raw.y < 0.0F ? 0.0F : raw.y}; // MUTATION")],
)

mutation(
    "overlap-uses-the-extent",
    "`penetration` returns the shared extent, undoing the containment fix.",
    "collision_test: 'containment is where penetration differs from the shared extent' "
    "(9 against 6) and the nested-resolution group, which would leave the body 3 pixels "
    "inside.",
    [patch(AABB,
           "        return Vec2{(m_halfExtents.x + other.m_halfExtents.x) - distanceX(other),\n"
           "                    (m_halfExtents.y + other.m_halfExtents.y) - distanceY(other)};",
           "        return Vec2{overlapX(other), overlapY(other)}; // MUTATION: the extent")],
)

mutation(
    "overlap-not-symmetric",
    "The x penetration measures the distance from this box's centre only, so the two "
    "argument orders disagree.",
    "collision_test: 'reversed argument order gives the same geometry', and every "
    "group that asks about a pair from both sides.",
    [patch(AABB,
           "    [[nodiscard]] constexpr float distanceX(const Aabb& other) const noexcept\n"
           "    {\n"
           "        const float delta = m_center.x - other.m_center.x;",
           "    [[nodiscard]] constexpr float distanceX(const Aabb& other) const noexcept\n"
           "    {\n"
           "        const float delta = m_center.x; // MUTATION: not the difference")],
)

# ---------------------------------------------------------------------------
# getPreviousOverlap: the previous position
# ---------------------------------------------------------------------------

mutation(
    "previous-uses-current-position",
    "`getPreviousOverlap` measures at the current position, so it returns the same "
    "number as `getOverlap`.",
    "collision_test: the first-frame group, the frame-to-frame group and the "
    "pre-correction group - and through the system, every `landedOn`, because a landing "
    "needs a previous vertical overlap at or below zero and a current-position reading "
    "gives a positive one. The player would never be grounded.",
    [patch(COLLISION_C,
           "    return Aabb{transform->prevPosition, collider->size};",
           "    return Aabb{transform->position, collider->size}; // MUTATION: not previous")],
)

mutation(
    "previous-position-written-after-integration",
    "`prevPosition` is written *after* the integration instead of before it, so it means "
    "'where the body was at the end of last frame' - a different quantity.",
    "collision_test: the frame-to-frame group requires each previous overlap to be the "
    "previous frame's current overlap, and 'the current movement does not overwrite the "
    "previous state early' requires the recorded penetration to be the one the step "
    "found rather than the corrected one.",
    [patch(PHYSICS_C,
           "        transform.prevPosition = transform.position;\n",
           "        // MUTATION: written after the integration instead\n")],
)

mutation(
    "previous-position-skipped-for-static",
    "`prevPosition` is written only for bodies that move, so a tile's previous position "
    "stays at whatever its initialisation left there.",
    "This is the real bug the phase hit: every previous overlap between a player and a "
    "tile was measured against a tile at the origin, so no player was ever grounded and "
    "none could jump. player_test catches it immediately.",
    [patch(PHYSICS_C,
           "        transform.prevPosition = transform.position;\n"
           "\n"
           "        if (const Body* const body = entity.tryGetConstComponent<Body>();",
           "        if (const Body* const body = entity.tryGetConstComponent<Body>();")],
)

mutation(
    "previous-position-never-written",
    "`prevPosition` is never written at all, so it stays at the `{0, 0}` that brace "
    "initialisation gives it.",
    "The first-frame group asserts a previous overlap of 5 for a pair that is 5 into "
    "each other; with no write this is measured against the origin. And the player never "
    "lands.",
    [patch(PHYSICS_C,
           "        transform.prevPosition = transform.position;\n",
           "        // MUTATION: never written\n")],
)

# ---------------------------------------------------------------------------
# The absent-box contract
# ---------------------------------------------------------------------------

mutation(
    "absent-previous-overlap-not-zero",
    "`getPreviousOverlap` returns a large positive number when a box is absent, which "
    "under this engine's sign convention is indistinguishable from a deep overlap.",
    "collision_test: 'an absent collider is reported as zero overlap'. It is also the "
    "unsafe direction by design: a consumer testing `overlap.y > 0` would read every "
    "uncollidable entity as a floor.",
    [patch(COLLISION_C,
           "    if (!firstBox.has_value() || !secondBox.has_value())\n"
           "    {\n"
           "        return Vec2{0.0F, 0.0F};\n"
           "    }\n"
           "\n"
           "    return firstBox->penetration(*secondBox);",
           "    if (!firstBox.has_value() || !secondBox.has_value())\n"
           "    {\n"
           "        return Vec2{1000.0F, 1000.0F}; // MUTATION: not zero\n"
           "    }\n"
           "\n"
           "    return firstBox->penetration(*secondBox);")],
)

mutation(
    "absent-overlap-treated-as-an-origin-point",
    "An absent box is answered for as an inert zero-size box at the origin rather than "
    "as absent.",
    "collision_test: 'an absent collider is reported as zero overlap' and 'a destroyed "
    "entity is treated as absent'.\n"
    "\n"
    "The first version of this mutation removed the null check outright, which segfaulted "
    "and came out CRASHED - a real defect in the code, but a crash is not a test failure, "
    "so it proved nothing about the tests. This version is the mistake a person would "
    "actually make: treat a missing box as a point at the origin, which returns a "
    "plausible wrong answer and is caught cleanly.",
    [patch(COLLISION_C,
           "        return std::nullopt;\n"
           "    }\n"
           "\n"
           "    return Aabb{transform->position, collider->size};",
           "        // MUTATION: an absent box is treated as an inert point at the origin\n"
           "        return Aabb{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}};\n"
           "    }\n"
           "\n"
           "    return Aabb{transform->position, collider->size};")],
)

# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------

mutation(
    "no-report-generated",
    "Collisions are detected and resolved but never recorded, so the report is always "
    "empty.",
    "player_test: every group that asserts a player is grounded, which is most of them. "
    "A player that is never grounded cannot jump, cannot land and cannot stand.",
    [patch(PHYSICS_C, "            m_collisions.add(record);",
           "            static_cast<void>(record); // MUTATION: not recorded")],
)

mutation(
    "report-not-cleared",
    "The report is never cleared, so each frame's results accumulate.",
    "collision_test: 'a collision from one frame is gone in the next', 'the report is "
    "empty before anything has run' and 'a destroyed entity does not survive into the "
    "next frame'. A stale landing would ground a player who is in the air.",
    [patch(PHYSICS_C, "    m_collisions.clear();",
           "    // MUTATION: never cleared")],
)

mutation(
    "report-cleared-after-the-step",
    "The report is cleared at the *end* of the step rather than the top, so the "
    "reconciliation that runs afterwards reads nothing.",
    "player_test: every grounded assertion. The consumer runs after physics, so clearing "
    "after the collision pass empties the report before anybody can read it.",
    [patch(PHYSICS_C, "    m_collisions.clear();",
           "    // MUTATION: moved"),  # replaced below by appending a clear at the end
     patch(PHYSICS_C,
           "            m_collisions.add(record);\n        }\n    }\n}",
           "            m_collisions.add(record);\n        }\n    }\n\n    m_collisions.clear(); // MUTATION: at the end\n}")],
)

mutation(
    "report-reads-the-corrected-overlap",
    "The record's current overlap is measured after the bodies have been pushed apart, "
    "so it reads the separation rather than the penetration.",
    "collision_test: 'the current movement does not overwrite the previous state early' "
    "requires 5 where a corrected read gives 0. And `landedOn` needs a positive current "
    "overlap, so the player stops being grounded.",
    [patch(PHYSICS_C,
           "            m_collisions.add(record);",
           "            // MUTATION: re-measured after the correction\n"
           "            record.overlap = boxOf(firstTransform, firstCollider)\n"
           "                                  .penetration(boxOf(secondTransform, secondCollider));\n"
           "            m_collisions.add(record);")],
)

# ---------------------------------------------------------------------------
# Resolution, and its consistency with detection
# ---------------------------------------------------------------------------

mutation(
    "vertical-resolution-disabled",
    "A pair is always pushed apart horizontally, so a body can never come to rest on "
    "anything.",
    "player_test: 'landing is recognised on the frame it happens' and 'a standing player "
    "is grounded on every single frame'. The player falls straight through the floor.",
    [patch(AABB,
           "        return depth.x < depth.y ? PenetrationAxis::Horizontal : PenetrationAxis::Vertical;",
           "        static_cast<void>(depth);\n"
           "        return PenetrationAxis::Horizontal; // MUTATION: never vertical")],
)

mutation(
    "horizontal-resolution-disabled",
    "A pair is always pushed apart vertically, so nothing can be blocked horizontally.",
    "player_test: 'a wall stops horizontal movement' and 'a wall in mid-air does not "
    "ground the player' - the player walks through the wall.",
    [patch(AABB,
           "        return depth.x < depth.y ? PenetrationAxis::Horizontal : PenetrationAxis::Vertical;",
           "        static_cast<void>(depth);\n"
           "        return PenetrationAxis::Vertical; // MUTATION: never horizontal")],
)

mutation(
    "minimum-translation-uses-the-extent-again",
    "`minimumTranslation` pushes by the shared extent again, so a nested body is left "
    "overlapping and the axis and the distance come from different numbers.",
    "collision_test: the nested-resolution group, which requires the body to end at "
    "x = 13 and not overlapping.",
    [patch(AABB,
           "        const Vec2 depth = penetration(other);\n"
           "        const float horizontal = depth.x;\n"
           "        const float vertical = depth.y;",
           "        const float horizontal = overlapX(other); // MUTATION: the extent\n"
           "        const float vertical = overlapY(other);")],
)

mutation(
    "downward-velocity-not-resolved",
    "The velocity component directed into a surface is no longer zeroed, so a body keeps "
    "its impact speed after landing.",
    "player_test: 'landing is recognised on the frame it happens' asserts the vertical "
    "velocity is zero on the landing frame and for thirty frames after, and a grounded "
    "player would accumulate speed again immediately.",
    [patch(PHYSICS_C,
           "    if (correction.y != 0.0F && (velocity.y * correction.y) < 0.0F)\n"
           "    {\n"
           "        velocity.y = 0.0F;\n"
           "    }",
           "    // MUTATION: vertical velocity survives the impact")],
)

# ---------------------------------------------------------------------------
# The classification
# ---------------------------------------------------------------------------

mutation(
    "landing-drops-the-previous-vertical-clause",
    "`landedOn` stops requiring the previous vertical overlap to be at or below zero, so "
    "a wall reads as a floor.",
    "This is the single most important clause in the phase. player_test: 'a wall in "
    "mid-air does not ground the player' and collision_test: 'a wall is not a landing' "
    "and 'landing, ceiling and side partition every collision'.",
    [patch(COLLISION_C,
           "    const bool arrivedFromOutside = collision.previousOverlap.x > 0.0F && collision.previousOverlap.y <= 0.0F;\n"
           "    const bool nowPenetratingVertically = collision.overlap.y > 0.0F;\n"
           "\n"
           "    // And the push was upward",
           "    const bool arrivedFromOutside = collision.previousOverlap.x > 0.0F; // MUTATION\n"
           "    const bool nowPenetratingVertically = collision.overlap.y > 0.0F;\n"
           "\n"
           "    // And the push was upward")],
)

mutation(
    "landing-drops-the-static-partner-clause",
    "`landedOn` stops requiring a static partner, so a player rests on a dynamic body.",
    "player_test: 'a dynamic body is not a floor and a static one is' and collision_test: "
    "'landing requires a static partner'.",
    [patch(COLLISION_C,
           "    return arrivedFromOutside && nowPenetratingVertically && pushedUp && hasStaticPartner(collision, entity);",
           "    return arrivedFromOutside && nowPenetratingVertically && pushedUp; // MUTATION")],
)

mutation(
    "landing-drops-the-push-direction",
    "`landedOn` no longer requires the push to be upward, so hitting the underside of a "
    "platform counts as landing on it.",
    "player_test: 'a ceiling stops a jump without grounding the player' asserts a ceiling "
    "is never also a landing, and collision_test: the partition group.",
    [patch(COLLISION_C,
           "    const bool pushedUp = resolutionFor(collision, entity).y < 0.0F;\n"
           "\n"
           "    return arrivedFromOutside && nowPenetratingVertically && pushedUp && hasStaticPartner(collision, entity);",
           "    return arrivedFromOutside && nowPenetratingVertically && hasStaticPartner(collision, entity); // MUTATION")],
)

mutation(
    "landing-ignores-who-is-asking",
    "`landedOn` stops checking that the caller is one of the two participants.",
    "PROVEN EQUIVALENT: the check cannot be observed, because `hasStaticPartner` is "
    "evaluated in the same conjunction and independently returns false for an entity that "
    "is in no collision at all. So the last clause already refuses every case the "
    "participant check refuses.\n"
    "\n"
    "collision_test's bystander group was written specifically to reach it and could not, "
    "which is what turned this from a suspected gap into a proof. The check is kept as "
    "defence in depth and the redundancy is now written next to it in Collision.cpp - "
    "dropping the static-partner clause must not turn into dropping both.\n"
    "\n"
    "The companion mutation below removes `hitCeilingWith`'s participant check, and that "
    "one *is* detected - the ceiling case has no static-partner clause to mask it.",
    [patch(COLLISION_C,
           "bool landedOn(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (!isParticipant(collision, entity))\n"
           "    {\n"
           "        return false;\n"
           "    }\n",
           "bool landedOn(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (false) // MUTATION: no participant check\n"
           "    {\n"
           "        return false;\n"
           "    }\n")],
    equivalent="`hasStaticPartner` is the final clause of the same conjunction and returns "
               "false for any entity that is not in the collision, so it refuses every case "
               "the participant check refuses. Redundant defence, kept and documented.",
)

mutation(
    "ceiling-ignores-who-is-asking",
    "`hitCeilingWith` stops checking that the caller is one of the two participants.",
    "collision_test: the bystander group in the partition test, which asks all three "
    "predicates about an entity that is in no collision. Unlike [landedOn] there is no "
    "static-partner clause here to mask the removal, so the ceiling test is the one that "
    "actually pins it.",
    [patch(COLLISION_C,
           "bool hitCeilingWith(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (!isParticipant(collision, entity))\n"
           "    {\n"
           "        return false;\n"
           "    }\n",
           "bool hitCeilingWith(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (false) // MUTATION: no participant check\n"
           "    {\n"
           "        return false;\n"
           "    }\n")],
)

mutation(
    "ceiling-and-landing-are-the-same-test",
    "`hitCeilingWith` stops requiring the push to be downward, so a ceiling reads as a "
    "landing too and the three outcomes no longer partition.",
    "collision_test: 'landing, ceiling and side partition every collision' requires "
    "exactly one of the three per collision, and player_test's ceiling group.",
    [patch(COLLISION_C,
           "    const bool pushedDown = resolutionFor(collision, entity).y > 0.0F;\n"
           "\n"
           "    return arrivedFromOutside && nowPenetratingVertically && pushedDown;",
           "    return arrivedFromOutside && nowPenetratingVertically; // MUTATION: same as a landing")],
)

mutation(
    "side-is-not-the-negation",
    "`hitSideWith` becomes an independent test rather than the negation of the other two, "
    "so a landing can also read as a side.",
    "collision_test: the partition group. The negation is what makes the three "
    "guaranteed to be exhaustive, and an independent test gives that up.",
    [patch(COLLISION_C,
           "    return isParticipant(collision, entity) && !landedOn(collision, entity) && !hitCeilingWith(collision, entity);",
           "    return isParticipant(collision, entity) && collision.previousOverlap.x <= 0.0F; // MUTATION")],
)

mutation(
    "static-partner-checks-the-wrong-one",
    "`hasStaticPartner` reports the entity's *own* body type rather than its partner's.",
    "player_test: 'a dynamic body is not a floor and a static one is'. A dynamic player "
    "would find itself standing on a dynamic box, and a static floor would find itself a "
    "floor.",
    [patch(COLLISION_C,
           "bool hasStaticPartner(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (collision.first == entity)\n"
           "    {\n"
           "        return collision.secondBody == BodyType::Static;\n"
           "    }\n"
           "\n"
           "    if (collision.second == entity)\n"
           "    {\n"
           "        return collision.firstBody == BodyType::Static;\n"
           "    }",
           "bool hasStaticPartner(const Collision& collision, const EntityId entity) noexcept\n"
           "{\n"
           "    if (collision.first == entity)\n"
           "    {\n"
           "        return collision.firstBody == BodyType::Static; // MUTATION: itself\n"
           "    }\n"
           "\n"
           "    if (collision.second == entity)\n"
           "    {\n"
           "        return collision.secondBody == BodyType::Static;\n"
           "    }")],
)

mutation(
    "resolution-for-does-not-negate",
    "`resolutionFor` returns the stored vector for either participant, so whichever body "
    "was visited second is told it was pushed the wrong way.",
    "player_test: everything that lands. The push a landing test reads is the sign, and "
    "an un-negated one points the wrong way for half of all pairs - which, because the "
    "iteration order decides, is a coin flip rather than a consistent failure.",
    [patch(COLLISION_C,
           "    if (collision.first == entity)\n"
           "    {\n"
           "        return collision.resolution;\n"
           "    }\n"
           "\n"
           "    return -collision.resolution;",
           "    if (collision.first == entity || collision.second == entity)\n"
           "    {\n"
           "        return collision.resolution; // MUTATION: never negated\n"
           "    }\n"
           "\n"
           "    return collision.resolution;")],
)

# ---------------------------------------------------------------------------
# The reconciliation
# ---------------------------------------------------------------------------

mutation(
    "state-system-not-registered",
    "`PlayerStateSystem` is not registered, so nothing ever reconciles the player's "
    "state and the player is never grounded.",
    "player_test: every grounded assertion, and scene_test's system count and order.",
    [patch(SCENE_C,
           "    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());\n", "")],
)

mutation(
    "state-system-after-the-camera",
    "`PlayerStateSystem` is registered after `CameraSystem` instead of immediately after "
    "physics, so the camera follows the player from the previous frame's state.",
    "scene_test pins the whole order by name *and* index, and this is the neighbouring "
    "position - a reorder that keeps every system's dependencies satisfied and still "
    "breaks the frame, which a count could never see.\n"
    "\n"
    "The version of this mutation that moved the reconciliation *before* physics is not "
    "expressible, and that is worth recording rather than forcing: `PlayerStateSystem` "
    "needs a reference to the physics system's report, and the only way to obtain one is "
    "`SystemManager::add`, which also registers. So the report's owner is necessarily "
    "registered first, and 'reconcile before physics' is unrepresentable in this engine "
    "rather than merely discouraged. This is the nearest ordering mistake that *can* be "
    "written.",
    [patch(SCENE_C,
           "    // Immediately after physics, and that position is the phase. The player's `grounded`\n"
           "    // flag is read from the collisions this frame resolved, so registering it earlier\n"
           "    // would put it a frame behind and reintroduce exactly the lag the collision report\n"
           "    // exists to remove. See PlayerStateSystem's own documentation.\n"
           "    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    // MUTATION: the reconciliation now runs after the camera\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());")],
)

mutation(
    "grounded-set-from-any-collision",
    "`PlayerStateSystem` treats any collision the player is involved in as support, "
    "instead of asking whether it was a landing.",
    "player_test: 'a wall in mid-air does not ground the player' and 'a ceiling stops a "
    "jump without grounding the player' - the brief's exact prohibition on 'there exists "
    "some nearby entity'.",
    [patch(STATE_C,
           "            if (landedOn(collision, entity.id()))",
           "            if (isParticipant(collision, entity.id())) // MUTATION: any collision")],
)

mutation(
    "jump-flag-never-cleared",
    "A landing does not clear the jump flag, so the player stays in Air and the variable "
    "jump can be cut by a jump from before the landing.",
    "player_test: 'landing returns to a grounded state' and 'each state selects its own "
    "animation'.",
    [patch(STATE_C,
           "        if (player.grounded)\n"
           "        {\n"
           "            player.jumping = false;\n"
           "        }",
           "        // MUTATION: the jump flag survives the landing")],
)

# ---------------------------------------------------------------------------
# Gravity, and the resting contact it makes possible
# ---------------------------------------------------------------------------

mutation(
    "gravity-conditional-again",
    "Gravity is applied only while the player is not grounded, which is the Phase 16 "
    "behaviour.",
    "With gravity withheld while grounded, a resting player never re-penetrates, so the "
    "next frame's collision pass sees a pure touch - which is not a collision - and "
    "grounded flickers. player_test: 'a standing player is grounded on every single "
    "frame' counts the frames and requires 300 of 300.",
    [patch(PLAYER_C,
           "        transform.velocity.y += config.gravity * deltaSeconds;",
           "        if (!player.grounded)\n"
           "        {\n"
           "            transform.velocity.y += config.gravity * deltaSeconds;\n"
           "        } // MUTATION: conditional again")],
)

mutation(
    "state-uses-grounded-alone",
    "The state machine decides Air from `grounded` alone, ignoring the jump flag.",
    "PROVEN EQUIVALENT, and the reason is written next to the line it edits.\n"
    "\n"
    "collision_test: 'the jump frame is airborne immediately' - which Phase 16 needed the "
    "flag for, and which now passes because the player is genuinely unsupported.",
    [patch(STATE_C,
           "        const bool airborne = !player.grounded || player.jumping;",
           "        const bool airborne = !player.grounded; // MUTATION: flag ignored")],
    equivalent="With collision-based grounding the launch frame's integration carries the "
               "player clear of the tile before the reconciliation reads the report, so "
               "there is no support collision that frame and `grounded` is already false. "
               "Phase 16 needed `|| player.jumping` only because its probe still found the "
               "tile being left. The flag is kept for the variable jump in PlayerSystem "
               "and as the honest reading, but in this expression it is redundant.",
)

# ---------------------------------------------------------------------------
# The update order
# ---------------------------------------------------------------------------

mutation(
    "player-system-after-physics",
    "`PlayerSystem` is registered *after* the physics step, so the velocity it writes is "
    "integrated on the following frame.",
    "player_test: 'player behaviour lands in the same frame as the input' moves the player "
    "by exactly `speed * dt` in one frame, and scene_test pins the order by index.\n"
    "\n"
    "The first version of this mutation only rewrote the explanatory comment and left the "
    "registration where it was, so it changed nothing at all and would have been recorded "
    "as an unexplained miss. It now genuinely moves the `add` call.",
    [patch(SCENE_C,
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n"
           "\n"
           "    // `add` hands back a reference",
           "    // MUTATION: the player's intent is registered after the integration\n"
           "    const engine::systems::PhysicsSystem& physics = m_systems.add<engine::systems::PhysicsSystem>();\n"
           "    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());\n"
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n"
           "\n"
           "    // `add` hands back a reference"),
     patch(SCENE_C,
           "    const engine::systems::PhysicsSystem& physics = m_systems.add<engine::systems::PhysicsSystem>();\n"
           "\n"
           "    // Immediately after physics, and that position is the phase. The player's `grounded`",
           "    // Immediately after physics, and that position is the phase. The player's `grounded`")],
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

    abnormal = {}
    for name, reason in re.findall(r"^\s*\d+ - (\S+) \((\S+(?: \S+)*)\)", output, re.MULTILINE):
        if reason != "Passed":
            abnormal[name] = reason

    timeouts = bool(re.search(r"\*\*\*Timeout|\bTimeout\b", output))
    clean_failed = [name for name, reason in abnormal.items() if reason == "Failed"]
    abnormal = {name: reason for name, reason in abnormal.items() if reason != "Failed"}

    if sorted(ran) != sorted(expected):
        raise RuntimeError("harness ran a different set of suites.\n  ran:     {}\n  expected: {}".format(
            sorted(ran), sorted(expected)))

    return clean_failed, abnormal, timeouts, output


def run_game():
    binary = os.path.join(BUILD, "game")
    if not os.path.exists(binary):
        return None, "no game binary"
    try:
        result = subprocess.run([binary, "--frames", "5"], cwd=ROOT, capture_output=True,
                                text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    return result.returncode, (result.stdout + result.stderr)


def failing_checks(output):
    found = {}
    for match in re.finditer(r"^\s*(\S+\.cpp):(\d+): CHECK\((.*?)\) failed", output, re.MULTILINE):
        found.setdefault(os.path.basename(match.group(1)), []).append(
            "{}:{}".format(match.group(2), match.group(3)))
    return found


def classify(clean_failed, abnormal, build_ok, timeouts, equivalent):
    """The five outcomes, decided by what the run actually did.

    A clean `Failed` outranks a crash *somewhere else*: declaring a three-frame run
    strip makes the asset manager throw from its constructor, so every suite that
    builds one aborts, and several also fail with real assertion failures first.
    Reporting CRASHED alone would throw those detections away; reporting
    TEST_DETECTED alone would hide the aborts. CRASHED is reserved for the case where
    nothing managed to assert anything.
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
        clean_failed, abnormal = [], {}
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
            else:
                clean_failed, abnormal, timeouts, test_output = run_all_tests(expected)
                game_code, _ = run_game()
                if game_code is not None and game_code != 0:
                    abnormal["game"] = "exit {}".format(game_code)
                outcome = classify(clean_failed, abnormal, built, timeouts, entry.get("equivalent"))
                detail = json.dumps({"clean_failed": clean_failed, "abnormal": abnormal,
                                     "game_exit": game_code,
                                     "equivalent": entry.get("equivalent")})
                detail += "\n    " + json.dumps(failing_checks(test_output))[:900]
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
