#!/usr/bin/env python3
"""Mutation testing for Phase 18, combat and tile behaviour.

Inherits the discipline of `tools/mutate_phase17.py`, and every item in that header
still applies:

* A patch that matches nothing produces a green run and would be recorded as EQUIVALENT,
  which is the most expensive way for a mutation harness to be wrong. Every patch is verified
  to have matched the expected number of times *and* to have survived write-back.
* Reverts go through `git checkout`, never string replacement: a deletion mutation has an
  empty `new`, and `str.replace("", old)` splices `old` between every character.
* The post-revert tree must rebuild and be green before the next mutation, or a clean
  mutation is judged against a stale binary.
* A crash is not a detection. ctest reports a segfault as `SEGFAULT`, not `Failed`, and a
  harness that matches only `Failed` files every segfault as UNDETECTED. Both reasons are
  captured and bucketed as CRASHED - but a clean `Failed` outranks a crash *elsewhere*, so a
  mutation that fails eight suites and aborts a ninth is still TEST_DETECTED with the abort
  recorded beside it.
* The suite list is read from `ctest -N` and compared.
* A mutation can be a **proven** equivalence, and the harness is given a place to say why.
  Leaving a proven equivalence in UNDETECTED makes it indistinguishable from an unexplained
  miss.

Phase 18 adds three traps of its own, and each one cost something while the suite was being
written:

* `TileSystem` destroys entities **inside** the walk that found them, so an entity flagged
  dead has already stopped matching queries while the report still names it. Removing a
  `destroyEntity` call does not hang and does not crash - it leaks, which nothing noticed
  until the lifetime groups started counting survivors rather than trusting the process to
  fail.
* Ordering is invisible to a count. `LifetimeSystem` after `TileSystem` keeps every group
  green except one; a brick that answers only to bullets keeps every group green except one.
  Both are here.
* The brick's collider comes off the moment it explodes, which makes the brick's `activated`
  flag genuinely unobservable. That is a real equivalence rather than a testing gap, and it is
  reported as one with the reason written down instead of being quietly omitted.
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
    os.path.join(tempfile.gettempdir(), "phase18-mutation-results.json"))

INC = "include/engine"
SRC = "src/engine"

TILE_H = f"{INC}/components/Tile.hpp"
SHOOT_C = f"{SRC}/systems/ShootSystem.cpp"
LIFETIME_C = f"{SRC}/systems/LifetimeSystem.cpp"
TILE_C = f"{SRC}/systems/TileSystem.cpp"
LOADER_C = f"{SRC}/level/LevelLoader.cpp"
SCENE_C = f"{SRC}/scene/PlayScene.cpp"
LEVEL_TXT = "assets/levels/level1.txt"
ASSETS_TXT = "assets/assets.txt"

MUTATIONS = []


def mutation(name, description, intent, patches, equivalent=None):
    MUTATIONS.append({"name": name, "description": description, "intent": intent,
                      "patches": patches, "equivalent": equivalent})


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


# ---------------------------------------------------------------------------
# Shooting
# ---------------------------------------------------------------------------

mutation(
    "shoot-becomes-level-triggered",
    "The shoot edge is replaced with the held state, so holding Space fires a bullet every "
    "frame.",
    "combat_test: 'holding Space does not create a bullet every frame', which holds the key "
    "for thirty frames and counts. At sixty frames a second this would make sixty bullets a "
    "second, every one of them a correctly spawned entity behaving correctly.\n"
    "\n"
    "It is also the mistake the course's own sentence warns about: *'If the jump key is held, "
    "the player should not continuously jump, but instead it should only jump once per button "
    "press.'*",
    [patch(SHOOT_C,
           "    if (!actions.wasPressed(Action::Shoot))",
           "    if (!actions.isActive(Action::Shoot)) // MUTATION: level, not edge")],
)

mutation(
    "shoot-never-fires",
    "The shoot action is refused, so nothing is ever created whatever the player does.",
    "combat_test: every shooting group, plus the two that fire a bullet to break a brick.\n"
    "\n"
    "Written as a **negated** guard rather than as a second copy of the same one: the first "
    "version of this mutation added a duplicate `if (wasPressed) return;` immediately after the "
    "original, which changed nothing at all and came out UNDETECTED. A mutation that is a "
    "no-op is worse than no mutation, because it looks like coverage.",
    [patch(SHOOT_C,
           "    if (!actions.wasPressed(Action::Shoot))\n    {\n        return;\n    }",
           "    if (actions.wasPressed(Action::Shoot)) // MUTATION: the press is refused\n    {\n"
           "        return;\n    }")],
)

mutation(
    "shoot-system-not-registered",
    "`ShootSystem` is not registered, so nothing is ever fired.",
    "combat_test: the shooting groups, and scene.lifecycle's system count and order.",
    [patch(SCENE_C, "    m_systems.add<engine::systems::ShootSystem>(context.assets());\n", "")],
)

mutation(
    "bullet-direction-inverted",
    "The bullet always travels right, whatever way the player is facing.",
    "combat_test: 'shooting left puts the bullet to the left and it goes left', and 'a bullet "
    "keeps the direction it was fired in' - which turns the player around mid-flight and reads "
    "the bullet's velocity again.",
    [patch(SHOOT_C,
           "[[nodiscard]] float facingOf(const Vec2& scale) noexcept { return scale.x < 0.0F ? -1.0F : 1.0F; }",
           "[[nodiscard]] float facingOf(const Vec2& scale) noexcept\n{\n"
           "    static_cast<void>(scale);\n"
           "    return 1.0F; // MUTATION: always right\n}")],
)

mutation(
    "bullet-speed-ignores-the-level",
    "The bullet's speed is a literal instead of the level's own speed times the course's "
    "factor.",
    "combat_test: 'the bullet speed is the level's speed times six', which compares against "
    "`kLevelLeftRightSpeed * 6` rather than against a number, so a group that only checked "
    "*which way* the bullet went would pass.\n"
    "\n"
    "The constant is 900 rather than 1200 on purpose: 1200 is exactly what this level's own "
    "numbers produce, and a mutation that changes nothing is not a mutation.",
    [patch(SHOOT_C,
           "        request.velocity = Vec2{facing * (config.leftRightSpeed * kBulletSpeedFactor), 0.0F};",
           "        static_cast<void>(config);\n"
           "        request.velocity = Vec2{facing * 900.0F, 0.0F}; // MUTATION: a constant")],
)

mutation(
    "bullet-speed-factor-dropped",
    "The bullet travels at the player's own speed rather than six times it.",
    "combat_test: 'the bullet speed is the level's speed times six'. The speed is still "
    "proportional to the level, so a group that only checked the *sign* would pass this one.",
    [patch(SHOOT_C, "constexpr float kBulletSpeedFactor = 6.0F;",
           "constexpr float kBulletSpeedFactor = 1.0F; // MUTATION: not the course's six")],
)

mutation(
    "bullet-spawn-offset-halved",
    "The bullet is created 32 pixels in front of the player rather than the course's 64.",
    "combat_test: both firing groups, which compare the bullet's position against "
    "`playerX +- 64 + one frame of travel`.",
    [patch(SHOOT_C, "constexpr float kBulletSpawnOffset = 64.0F;",
           "constexpr float kBulletSpawnOffset = 32.0F; // MUTATION: half the course's")],
)

mutation(
    "bullet-spawn-offset-is-arbitrary",
    "The bullet is created 600 pixels in front of the player - a large constant that is "
    "neither the course's 64 nor anything derived from the player's own box.",
    "combat_test: the same two groups, and the collision groups - a bullet spawned at 620 is "
    "already past the wall these fixtures put at 192, so it never meets one.\n"
    "\n"
    "A different *kind* of wrong number from the mutation above, on purpose: a smaller offset "
    "would also be caught, but for a different reason, and the brief asks that the offset not "
    "be an arbitrary large constant.",
    [patch(SHOOT_C, "constexpr float kBulletSpawnOffset = 64.0F;",
           "constexpr float kBulletSpawnOffset = 600.0F; // MUTATION: arbitrary")],
)

mutation(
    "bullet-spawned-after-physics",
    "`ShootSystem` is registered *after* the physics step, so a bullet waits a frame for its "
    "first move and cannot be in this frame's collision report.",
    "combat_test: 'the bullet moves in the frame it is fired' - which measures one frame of "
    "travel and gets zero - and the two firing groups, whose expected position includes a "
    "frame of travel.\n"
    "\n"
    "scene.lifecycle pins the order by name and index, and this is the neighbouring position: "
    "a reorder that keeps every system's stated dependencies satisfied and still breaks the "
    "frame, which a count could never see.",
    [patch(SCENE_C, "    m_systems.add<engine::systems::ShootSystem>(context.assets());\n", ""),
     patch(SCENE_C,
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());\n",
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());\n"
           "    // MUTATION: the shot is created after the integration\n"
           "    m_systems.add<engine::systems::ShootSystem>(context.assets());\n")],
)

mutation(
    "bullet-not-dynamic",
    "A bullet is created with a static body, so the physics step never integrates it.",
    "combat_test: 'the bullet moves in the frame it is fired' and 'a bullet that misses "
    "everything is still alive after many frames', which watches the bullet travel a hundred "
    "pixels.",
    [patch(SHOOT_C, "        bullet.addComponent<Body>(Body{BodyType::Dynamic});",
           "        bullet.addComponent<Body>(Body{BodyType::Static}); // MUTATION: never moves")],
)

mutation(
    "bullet-box-is-the-whole-frame",
    "The bullet's collider is the whole animation frame rather than half of it, which is what "
    "the course halves.",
    "combat_test: 'the bullet box is half its animation frame', which reads the real frame size "
    "from the asset manager and halves it.",
    [patch(SHOOT_C, "        bullet.addComponent<Collider>(Collider{frameSize * 0.5F});",
           "        bullet.addComponent<Collider>(Collider{frameSize}); // MUTATION: not halved")],
)

mutation(
    "bullet-animation-does-not-repeat",
    "A bullet's animation does not repeat, so a one-frame animation is ended after a single "
    "tick and `AnimationSystem` destroys the bullet on the frame it was created.",
    "combat_test: 'the bullet is drawn with the level's bullet animation and never ends', and "
    "every collision group - the bullet is gone before it can reach anything.\n"
    "\n"
    "This is the mutation a `repeat == true` assertion on its own would have missed, which is "
    "why that group steps five frames and counts bullets rather than reading the flag.",
    [patch(SHOOT_C,
           "        bullet.addComponent<Animation>(Animation{request.animationName, 0U, 0U, true, false});",
           "        bullet.addComponent<Animation>(Animation{request.animationName, 0U, 0U, false, false}); "
           "// MUTATION: ends at once")],
)

mutation(
    "bullet-has-no-lifetime",
    "A bullet is created with no [Lifetime] at all, so nothing ever expires it.",
    "combat_test: 'a bullet expires after its frames and not before', which steps to the last "
    "frame of its life and requires it to still be there, and 'a bullet lifetime is counted in "
    "frames not seconds'.",
    [patch(SHOOT_C, "        bullet.addComponent<Lifetime>(Lifetime{kBulletLifetimeFrames});",
           "        static_cast<void>(kBulletLifetimeFrames); // MUTATION: never expires")],
)

mutation(
    "bullet-lifetime-destroyed-at-birth",
    "A bullet is flagged for destruction as it is created, so it never travels.",
    "combat_test: every shooting group - the bullet is gone before anything reads it - and the "
    "collision groups.\n"
    "\n"
    "Also the mutation that exercises the deferred-destruction contract from the other side: "
    "the entity stops matching queries immediately, and the owner erases it at the end of the "
    "frame. Nothing here has to invalidate anything.",
    [patch(SHOOT_C, "        bullet.addComponent<Lifetime>(Lifetime{kBulletLifetimeFrames});",
           "        bullet.addComponent<Lifetime>(Lifetime{kBulletLifetimeFrames});\n"
           "        entities.destroyEntity(bullet); // MUTATION: dies at birth")],
)

# ---------------------------------------------------------------------------
# Bullet collision
# ---------------------------------------------------------------------------

mutation(
    "bullet-never-destroyed",
    "A bullet that collides with anything is not destroyed.",
    "combat_test: the three groups that destroy a bullet against a tile, 'a bullet cannot pass "
    "through a solid tile' and 'the bullet that died is one physics reported'.",
    [patch(TILE_C,
           "        if (struck)\n        {\n            entities.destroyEntity(entity);\n        }",
           "        static_cast<void>(struck); // MUTATION: the bullet lives")],
)

mutation(
    "bullet-destroyed-on-sight",
    "A bullet is destroyed the frame it is created, whether or not it hit anything.",
    "combat_test: 'the bullet moves in the frame it is fired' and 'a bullet that misses "
    "everything is still alive after many frames'.\n"
    "\n"
    "The degenerate opposite of the mutation above, and the reason both are here: a suite that "
    "checked only that bullets die would pass this one.",
    [patch(TILE_C,
           "        if (struck)\n        {\n            entities.destroyEntity(entity);\n        }",
           "        entities.destroyEntity(entity); // MUTATION: always")],
)

mutation(
    "bullet-ignores-collisions",
    "A bullet's collisions are never recorded, so it can never meet anything.",
    "combat_test: the collision groups, and - separately - 'the bullet that died is one physics "
    "reported', which needs the record itself rather than only the death.\n"
    "\n"
    "Distinct from `bullet-never-destroyed` because this one still destroys the bullet on "
    "whatever `struck` says, and `struck` is now always false: the bullet leaks rather than "
    "surviving usefully. The two look identical from outside and are fixed in different "
    "places.",
    [patch(TILE_C,
           "            bulletHits.push_back(Pair{entity.id(), partnerOf(collision, entity.id())});\n"
           "            struck = true;",
           "            // MUTATION: the collision is not recorded")],
)

mutation(
    "bullet-collision-uses-its-own-aabb-test",
    "The bullet's death stops being decided by the collision report and becomes a proximity "
    "test against every tile in the world.",
    "combat_test: 'the bullet that died is one physics reported', which watches the report for "
    "a record naming both the bullet and the wall **on the frame the bullet vanished**. A "
    "proximity test produces exactly the same death and no record at all.\n"
    "\n"
    "This is the mutation the phase brief calls a second collision implementation, and it is "
    "the one that makes `CollisionReport` load-bearing rather than convenient. Every other "
    "bullet group still passes, because a hand-rolled AABB loop and the physics step agree on "
    "every case these fixtures contain.",
    [patch(TILE_C,
           "/// Whether anything in `pairs` names `id` as the thing that was hit from below.",
           "[[nodiscard]] Vec2 bulletBoxSize(const Entity& bullet)\n{\n"
           "    const Collider* const collider = bullet.tryGetConstComponent<Collider>();\n"
           "    return collider != nullptr ? collider->size : Vec2{0.0F, 0.0F};\n"
           "}\n\n"
           "/// Whether anything in `pairs` names `id` as the thing that was hit from below."),
     patch(TILE_C,
           "        bool struck = false;\n"
           "        for (const Collision& collision : report.collisions())\n"
           "        {\n"
           "            if (!isParticipant(collision, entity.id()))\n"
           "            {\n"
           "                continue;\n"
           "            }\n\n"
           "            bulletHits.push_back(Pair{entity.id(), partnerOf(collision, entity.id())});\n"
           "            struck = true;\n"
           "        }",
           "        bool struck = false;\n"
           "        for (const Collision& collision : report.collisions())\n"
           "        {\n"
           "            if (!isParticipant(collision, entity.id()))\n"
           "            {\n"
           "                continue;\n"
           "            }\n"
           "        }\n\n"
           "        // MUTATION: a proximity test instead of the report. It agrees with the\n"
           "        // physics step on every case these fixtures contain, which is exactly\n"
           "        // why it is dangerous and exactly why one group looks at the report.\n"
           "        const engine::physics::Aabb bulletBox{transform.position, bulletBoxSize(entity)};\n"
           "        for (const Entity& other : entities.getEntities())\n"
           "        {\n"
           "            const Collider* const collider = other.tryGetConstComponent<Collider>();\n"
           "            const Transform* const at = other.tryGetConstComponent<Transform>();\n"
           "            if (collider == nullptr || at == nullptr || other.id() == entity.id() ||\n"
           "                !other.hasComponent<Tile>())\n"
           "            {\n"
           "                continue;\n"
           "            }\n\n"
           "            if (bulletBox.overlaps(engine::physics::Aabb{at->position, collider->size}))\n"
           "            {\n"
           "                bulletHits.push_back(Pair{entity.id(), other.id()});\n"
           "                struck = true;\n"
           "            }\n"
           "        }")],
)

mutation(
    "stale-report-consumed",
    "The pairs the tile system acts on are the *previous* frame's, kept in a static.",
    "combat_test: 'a bullet hitting a brick starts the explosion and destroys the bullet', "
    "'a bullet cannot pass through a solid tile' and every other group that waits for a "
    "collision to have an effect.\n"
    "\n"
    "One frame is enough to break all of it: on the first frame the carried list is empty, and "
    "from then on every effect lands on the frame after the one that caused it. The brief "
    "names this - *'Do not cache Collision references across frames'* - and the swap below is "
    "the mechanical version of the mistake.",
    [patch(TILE_C,
           "    std::vector<Pair> hitsFromBelow;",
           "    static std::vector<Pair> s_stale; // MUTATION: a frame-old report\n"
           "    bulletHits.swap(s_stale);\n\n"
           "    std::vector<Pair> hitsFromBelow;")],
)

mutation(
    "brick-ignores-bullets",
    "A bullet no longer breaks a brick, though it still stops against it.",
    "combat_test: 'a bullet hitting a brick starts the explosion and destroys the bullet' and "
    "'firing from the committed level breaks a brick'.\n"
    "\n"
    "Separate from `bullet-never-destroyed` because this is the half of the rule that is easy "
    "to break by accident: the bullet's *death* is not the brick's *behaviour*, and a suite "
    "that only counted bullets would not notice.",
    [patch(TILE_C,
           "            if (struckByBullet || hitFromBelow(hitsFromBelow, id))",
           "            if (hitFromBelow(hitsFromBelow, id)) // MUTATION: bullets do nothing")],
)

mutation(
    "brick-requires-both-triggers",
    "A brick needs a bullet **and** a player from below, so neither trigger alone fires it.",
    "combat_test: the bullet group and the from-below group separately. Written as `&&` "
    "against the production `||` because it is the mutation a well-meaning 'make sure both "
    "cases are covered' edit produces, and it breaks both.",
    [patch(TILE_C,
           "            if (struckByBullet || hitFromBelow(hitsFromBelow, id))",
           "            if (struckByBullet && hitFromBelow(hitsFromBelow, id)) // MUTATION")],
)

mutation(
    "bullet-uses-a-question-block",
    "A bullet that reaches a question block uses it as well as stopping against it.",
    "combat_test: 'a bullet does not use a question block', which is the whole of the "
    "interaction matrix for that pair. The course names the player's hit as the trigger, and "
    "'any hit works' would be a rule nobody chose.",
    [patch(TILE_C,
           "        if (hitFromBelow(hitsFromBelow, id))\n"
           "        {\n"
           "            useQuestionBlock(",
           "        if (hitFromBelow(hitsFromBelow, id) || struckByBullet) // MUTATION: any hit\n"
           "        {\n"
           "            useQuestionBlock(")],
)

mutation(
    "decoration-becomes-a-solid-target",
    "Decorations are given a collider and a body, so a bullet stops on a cloud.",
    "combat_test: 'a decoration is neither collidable nor a bullet target', which reads the "
    "cloud's components, checks it is never named in a collision record across forty frames, "
    "and watches the bullet fly past where it was.\n"
    "\n"
    "In the loader rather than in the combat systems, and that is the point: the alternative "
    "would be to teach the combat systems to exclude decorations, which is a second piece of "
    "knowledge about what a decoration is in a place with no business knowing.",
    [patch(LOADER_C,
           "            entity.addComponent<components::Animation>(loopingAnimation(decoration.animationName));",
           "            entity.addComponent<components::Animation>(loopingAnimation(decoration.animationName));\n"
           "            // MUTATION: a decoration is a solid gameplay target\n"
           "            entity.addComponent<components::Collider>(components::Collider{Vec2{70.0F, 51.0F}});\n"
           "            entity.addComponent<components::Body>(components::Body{physics::BodyType::Static});")],
)

# ---------------------------------------------------------------------------
# The brick
# ---------------------------------------------------------------------------

mutation(
    "landing-breaks-a-brick",
    "A player *landing* on a brick starts the explosion, as well as one arriving from below.",
    "combat_test: 'landing on a brick does not start the explosion', where the player is "
    "dropped onto the brick's top from grid row 5 and lands on it.\n"
    "\n"
    "Written as `landedOn` alongside `hitCeilingWith` rather than replacing it, because that is "
    "what a person confusing the two directions would actually write - and it is the whole "
    "reason Phase 17 has both.",
    [patch(TILE_C,
           "using engine::physics::hitCeilingWith;",
           "using engine::physics::hitCeilingWith;\nusing engine::physics::landedOn;"),
     patch(TILE_C,
           "            if (hitCeilingWith(collision, entity.id()))",
           "            if (hitCeilingWith(collision, entity.id()) || landedOn(collision, entity.id())) "
           "// MUTATION")],
)

mutation(
    "any-collision-breaks-a-brick",
    "A player colliding with a brick from *any* direction starts the explosion.",
    "combat_test: 'walking into the side of a brick does not start the explosion' and 'landing "
    "on a brick does not start the explosion' - two groups for one mutation, because the "
    "direction test is what keeps both of those quiet and each of them fails for its own "
    "reason.",
    [patch(TILE_C,
           "            if (hitCeilingWith(collision, entity.id()))",
           "            if (isParticipant(collision, entity.id())) // MUTATION: any direction")],
)

mutation(
    "brick-keeps-its-collider",
    "The brick's animation is pointed at the explosion but its collider is left in place, so "
    "the player is still blocked by something that no longer looks like a brick.",
    "combat_test: 'a bullet hitting a brick starts the explosion and destroys the bullet', "
    "which asserts the collider is gone, and 'a destroyed brick stops blocking the player'.",
    [patch(TILE_C,
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);\n"
           "    static_cast<void>(brick.tryRemoveComponent<Collider>());",
           "    static_cast<void>(brick); // MUTATION: still solid\n"
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);")],
)

mutation(
    "brick-explodes-without-the-animation",
    "The brick's collider is removed and it is marked activated, but no explosion animation is "
    "started - so nothing is ever seen and the entity stays forever.",
    "combat_test: 'a brick keeps its explosion running until the animation ends', which asserts "
    "the explosion's name and then walks it to its end, and 'a bullet hitting a brick starts "
    "the explosion'.\n"
    "\n"
    "The mirror image of `brick-keeps-its-collider`: one keeps the collider and loses the "
    "picture, the other does the reverse. Neither is caught by an assertion about the other "
    "half.",
    [patch(TILE_C,
           "    tile.activated = true;\n"
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);",
           "    tile.activated = true;\n"
           "    static_cast<void>(animation); // MUTATION: no explosion to see")],
)

mutation(
    "brick-explosion-repeats",
    "The brick's explosion animation repeats, so it never ends and the entity is never "
    "destroyed.",
    "combat_test: 'a brick keeps its explosion running until the animation ends', which watches "
    "the frame index for a wrap and counts frames until the entity is gone. It makes no "
    "assertion about the `repeat` flag at all, so no flag check could have caught this.",
    [patch(TILE_C,
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);",
           "    pointAnimationAt(animation, components::kExplosionAnimationName, true); // MUTATION: "
           "loops")],
)

mutation(
    "explosion-destroyed-immediately",
    "The explosion animation is marked ended on the frame it is started, so `AnimationSystem` "
    "removes the brick at once and the explosion is never seen.",
    "combat_test: 'a brick keeps its explosion running until the animation ends', which finds "
    "the frame the explosion starts and then counts - and finds the entity already gone.\n"
    "\n"
    "Written as a flag rather than as a call into the manager, because that is the reachable "
    "form: a tile system that destroyed the entity itself would have to reach past the frame "
    "it is in, and this version needs nothing new.",
    [patch(TILE_C,
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);\n"
           "    static_cast<void>(brick.tryRemoveComponent<Collider>());",
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);\n"
           "    animation.ended = true; // MUTATION: gone before it can be seen\n"
           "    static_cast<void>(brick.tryRemoveComponent<Collider>());")],
)

mutation(
    "explosion-ends-a-frame-early",
    "The explosion animation is marked ended one advance before the final frame has been held "
    "for its full period.",
    "combat_test: the same group, which requires the entity to survive exactly "
    "`frameCount * speed - 1` further steps after the frame the explosion started. The count "
    "is read from the asset, so it moves if the strip does.\n"
    "\n"
    "This is the mutation the phase brief asks for with *'Do not destroy the entity in the "
    "middle of iteration'*'s neighbour: the difference between 'the last frame is on screen' "
    "and 'the last frame has been seen'. [engine::components::Animation] draws that line and "
    "the group counts frames to see it.",
    [patch(TILE_C,
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);",
           "    pointAnimationAt(animation, components::kExplosionAnimationName, false);\n"
           "    animation.currentFrame = 11U; // MUTATION: jumps to the last frame")],
)

mutation(
    "explosion-never-destroyed",
    "The explosion animation is pointed at the brick but `activated` is left false, so the "
    "brick can be broken again and the explosion restarts - and a second bullet reaching the "
    "tile would start a second one.",
    "combat_test: 'a bullet hitting a brick twice does not restart the explosion'.\n"
    "\n"
    "Written as *not* setting the flag rather than as removing the destruction, because the "
    "destruction is `AnimationSystem`'s and is not this system's to remove.",
    [patch(TILE_C,
           "void startExplosion(Entity& brick, Animation& animation, Tile& tile)\n{\n    tile.activated = true;",
           "void startExplosion(Entity& brick, Animation& animation, Tile& tile)\n{\n"
           "    tile.activated = false; // MUTATION: can be broken again and again")],
)

mutation(
    "brick-activation-is-never-marked",
    "`activated` is never set on a brick, so nothing records that it has already exploded.",
    "combat_test: 'a bullet hitting a brick starts the explosion and destroys the bullet' and "
    "'the player hitting a brick from below starts the explosion', both of which read the flag "
    "back off the tile that exploded.\n"
    "\n"
    "This mutation was written expecting to be a **proven equivalence**, on the argument that a "
    "brick removes its collider the instant it explodes and so can never be hit twice. That "
    "argument is right about the *behaviour* and wrong about the *test*: the flag is the piece "
    "of state the whole idempotency argument rests on, and reading it back is exactly what a "
    "component is for. The behavioural groups - 'a bullet hitting a brick twice does not "
    "restart the explosion' and 'the used block stays used' - are what prove the guarantee; "
    "this one only proves the flag was set, which is a weaker and separate claim.",
    [patch(TILE_C,
           "void startExplosion(Entity& brick, Animation& animation, Tile& tile)\n{\n    tile.activated = true;",
           "void startExplosion(Entity& brick, Animation& animation, Tile& tile)\n{\n"
           "    static_cast<void>(tile); // MUTATION: never marked")],
)

mutation(
    "brick-identified-by-artwork",
    "A brick is recognised by comparing its animation name against the brick's, instead of "
    "reading the semantic tile type.",
    "combat_test: 'a brick is identified by its type not by its artwork', which hand-builds a "
    "**Brick-typed tile drawn with the ground tile's own artwork** and requires it to break. A "
    "name comparison leaves it alone, and every other group in the file still passes, because "
    "every other brick is drawn with the right picture.\n"
    "\n"
    "This is the mutation the phase brief asks for and the reason "
    "[engine::components::TileType] exists at all.",
    [patch(TILE_C,
           "        if (tile.type == TileType::Brick)",
           "        if (animation.assetName == std::string{components::kBrickAnimationName}) // MUTATION: "
           "by name")],
)

# ---------------------------------------------------------------------------
# The question block
# ---------------------------------------------------------------------------

mutation(
    "question-uses-on-any-collision",
    "A question block is used by the player colliding with it from any direction.",
    "combat_test: 'walking into the side of a question block does not use it' and 'landing on "
    "a question block does not use it'.",
    [patch(TILE_C,
           "            if (hitCeilingWith(collision, entity.id()))",
           "            if (isParticipant(collision, entity.id())) // MUTATION: any direction")],
)

mutation(
    "question-never-becomes-the-used-block",
    "The coin is dropped but the block keeps its unused type and its unused artwork.",
    "combat_test: 'the player hitting a question block from below uses it', which requires the "
    "type to become Question2 and the animation to become the used artwork, and 'the used "
    "block keeps its position and its collider', which asserts the same thing *before* its "
    "geometry - the first version of that group passed vacuously and now does not.",
    [patch(TILE_C,
           "    tile.activated = true;\n    tile.type = TileType::Question2;",
           "    tile.activated = true; // MUTATION: stays an unused Question")],
)

mutation(
    "question-repeats-its-activation",
    "The used block's type is left alone, so the player standing under it keeps dropping "
    "coins.",
    "combat_test: 'the used block stays used', which holds the player underneath for 180 "
    "frames and requires exactly one coin for the whole level and no change to the tile count.",
    [patch(TILE_C,
           "    tile.activated = true;\n    tile.type = TileType::Question2;",
           "    tile.type = TileType::Question; // MUTATION: reactivates forever")],
)

mutation(
    "used-question-does-not-loop",
    "The used block's animation does not repeat, so a one-frame animation is ended after one "
    "tick and `AnimationSystem` deletes the block out of the level.",
    "combat_test: 'the player hitting a question block from below uses it' and 'the used block "
    "stays used', which both run far past the frame on which a non-repeating single-frame "
    "animation ends.",
    [patch(TILE_C, "        pointAnimationAt(animation, used, true);",
           "        pointAnimationAt(animation, used, false); // MUTATION: deletes the block")],
)

# ---------------------------------------------------------------------------
# The coin
# ---------------------------------------------------------------------------

mutation(
    "coin-spawns-at-half-the-height",
    "The coin appears 32 pixels above the question block rather than the course's 64, which "
    "puts it inside the block.",
    "combat_test: 'using a question block creates exactly one coin', which compares the coin's "
    "centre against `blockCentre.y - 64`, and 'the coin is not inside the question block', "
    "which builds both boxes and asserts they do not overlap.",
    [patch(TILE_C, "constexpr float kCoinSpawnHeight = 64.0F;",
           "constexpr float kCoinSpawnHeight = 32.0F; // MUTATION: inside the block")],
)

mutation(
    "coin-spawns-below-not-above",
    "The coin appears 64 pixels *below* the question block, in this engine's downward Y.",
    "combat_test: the same two groups. Written as a sign flip rather than a different "
    "magnitude, because a sign flip is the mistake a person makes when the axis convention is "
    "wrong - and no other group in the file can see it.",
    [patch(TILE_C,
           "transform.position.y - kCoinSpawnHeight},\n                             coinSpawns);",
           "transform.position.y + kCoinSpawnHeight}, // MUTATION: below the block\n"
           "                             coinSpawns);")],
)

mutation(
    "coin-lifetime-changed",
    "The coin lives 15 frames rather than 30.",
    "combat_test: 'the coin survives its whole life and then goes' and 'the coin's lifetime is "
    "counted in frames not seconds', which step to frame 29 and require the coin to still be "
    "there.",
    [patch(TILE_C, "constexpr std::uint32_t kCoinLifetimeFrames = 30U;",
           "constexpr std::uint32_t kCoinLifetimeFrames = 15U; // MUTATION: too short")],
)

mutation(
    "coin-never-expires",
    "The coin is created with no lifetime at all, so it is in the world for the rest of the "
    "session.",
    "combat_test: 'the coin survives its whole life and then goes', 'a coin is not destroyed by "
    "the animation system' and 'a coin expiring while the world moves is safe'.\n"
    "\n"
    "It is also the reason the lifetime groups *count survivors* rather than asserting that "
    "something threw: an entity that never expires fails quietly and looks exactly like a suite "
    "that never ran the frames.",
    [patch(TILE_C, "    coin.addComponent<Lifetime>(Lifetime{kCoinLifetimeFrames});",
           "    static_cast<void>(kCoinLifetimeFrames); // MUTATION: never expires")],
)

mutation(
    "coin-expires-a-frame-early",
    "The countdown runs after the coin is created instead of before it, so a coin lives 29 "
    "frames rather than 30.",
    "combat_test: the two lifetime groups, which step to the last frame and require the coin "
    "to still be alive there. Every other group in the file is indifferent to it.\n"
    "\n"
    "The mutation is the *ordering*, not a number. `LifetimeSystem` moves to just after "
    "`TileSystem`, which is the nearest position that is expressible and the one a person "
    "would reach for while reordering a list.",
    [patch(SCENE_C, "    m_systems.add<engine::systems::LifetimeSystem>();\n", ""),
     patch(SCENE_C,
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    // MUTATION: the countdown now runs after the coin is created\n"
           "    m_systems.add<engine::systems::LifetimeSystem>();\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});")],
)

mutation(
    "coin-is-collidable",
    "The coin is given a collider and a body, so it takes part in the collision pass.",
    "combat_test: 'the coin is not collidable', which asserts both components are absent.\n"
    "\n"
    "A genuine defect rather than a hypothetical one: a coin with a box is a solid object in "
    "the world, and it is dropped 64 pixels above the block it came from.",
    [patch(TILE_C, "    coin.addComponent<Lifetime>(Lifetime{kCoinLifetimeFrames});",
           "    coin.addComponent<Lifetime>(Lifetime{kCoinLifetimeFrames});\n"
           "    // MUTATION: a coin you could stand on\n"
           "    coin.addComponent<Collider>(Collider{Vec2{16.0F, 16.0F}});\n"
           "    coin.addComponent<components::Body>(components::Body{physics::BodyType::Static});")],
)

mutation(
    "coin-destroyed-by-the-animation-system",
    "The coin's animation does not repeat, so a single-frame animation is ended after one "
    "tick and `AnimationSystem` destroys it on the frame it was created.",
    "combat_test: 'a coin is not destroyed by the animation system' - twenty frames of stepping "
    "with the coin counted after every one - and every group that waits for a coin to appear, "
    "which would wait forever and then fail.",
    [patch(TILE_C,
           "    coin.addComponent<Animation>(Animation{std::string{components::kCoinAnimationName}, 0U, 0U, "
           "true, false});",
           "    coin.addComponent<Animation>(Animation{std::string{components::kCoinAnimationName}, 0U, 0U, "
           "false, false}); // MUTATION: ends at once")],
)

# ---------------------------------------------------------------------------
# Entity lifetime
# ---------------------------------------------------------------------------

mutation(
    "lifetime-destroys-a-frame-early",
    "`LifetimeSystem` stops counting down one frame early, so a coin lasts 29 frames and a "
    "bullet 108.",
    "combat_test: the two coin lifetime groups and the two bullet lifetime groups, all of which "
    "step to the last frame of the entity's life and require it to still be there.\n"
    "\n"
    "The one line that decides whether '30 frames' means thirty frames or twenty-nine, and "
    "nothing else in the file can see it.",
    [patch(LIFETIME_C,
           "        --lifetime.remainingFrames;\n",
           "        if (lifetime.remainingFrames > 1U)\n        {\n"
           "            --lifetime.remainingFrames;\n        } // MUTATION: a frame early\n")],
)

mutation(
    "lifetime-system-not-registered",
    "`LifetimeSystem` is not registered, so nothing that carries a lifetime ever expires.",
    "combat_test: both coin lifetime groups, both bullet lifetime groups, and scene.lifecycle's "
    "count and order.\n"
    "\n"
    "It is worth running precisely because 'a coin is not destroyed by the animation system' "
    "*passes* without it - a coin that never expires is exactly what that group asserts - so "
    "the lifetime groups are the only thing standing between this mutation and the suite.",
    [patch(SCENE_C, "    m_systems.add<engine::systems::LifetimeSystem>();\n", "")],
)

mutation(
    "lifetime-destroys-on-the-first-frame",
    "Every entity carrying a lifetime is destroyed the frame it is seen, whatever the count "
    "says.",
    "combat_test: every shooting and collision group - a bullet is gone before it can move - "
    "and every coin group.\n"
    "\n"
    "The opposite end of `lifetime-destroys-a-frame-early`, and here because a countdown with "
    "its destruction condition replaced by an unconditional one is the mistake a person makes "
    "when they move the line into the wrong place in the loop.\n"
    "\n"
    "The first version of this mutation removed the zero guard instead, which is not a "
    "different mutation at all - it is `lifetime-guard-is-removed` again, and it came out "
    "UNDETECTED because the guard is unreachable. Duplicating an equivalence under a second "
    "name is how a harness ends up with two mutations and one fact.",
    [patch(LIFETIME_C,
           "        // Exactly on reaching zero, and not before. `remainingFrames` counts the frame it is\n"
           "        // on, so an entity with 30 spends frame one at 30, frame thirty at 1, and is\n"
           "        // destroyed during the frame that takes it from 1 to 0.\n"
           "        if (lifetime.remainingFrames == 0U)",
           "        // MUTATION: destroyed whatever the count says\n"
           "        if (lifetime.remainingFrames < 1000U)")],
)

mutation(
    "lifetime-guard-is-removed",
    "The guard against counting an entity whose lifetime has already run out is removed.",
    "PROVEN EQUIVALENT, and it is a real property of the current engine rather than of the "
    "tests.\n"
    "\n"
    "Nothing in the engine can produce an entity with a lifetime of zero. `ShootSystem` and "
    "`TileSystem` are the only writers, both build the component from a positive constant, and "
    "the one path that *reaches* zero is the line immediately above this one - which destroys "
    "the entity in the same pass, so the flag is set before the next frame is run and the owner "
    "erases it in between. The guard therefore never fires on any reachable path, and removing "
    "it changes nothing observable.\n"
    "\n"
    "It is kept as defence in depth, because `EntityManager::destroyEntity` throws on an "
    "already-dead entity rather than ignoring it, and because the guard is the difference "
    "between a later mistake being a crash and being a nothing. Reported as an equivalence "
    "rather than omitted, so the fact is on the record.",
    [patch(LIFETIME_C,
           "        if (lifetime.remainingFrames == 0U)\n        {\n            continue;\n        }\n\n        --lifetime.remainingFrames;",
           "        --lifetime.remainingFrames; // MUTATION: no guard against a run-out count")],
    equivalent="Nothing reachable produces a Lifetime of zero: both writers build it from a "
               "positive constant, and the only path that reaches zero is the line above, "
               "which destroys the entity in the same pass. The guard never fires and its "
               "removal is unobservable. Kept as defence in depth against `destroyEntity`'s "
               "throw.",
)

mutation(
    "tile-system-not-registered",
    "`TileSystem` is not registered, so no brick explodes and no question block is ever used.",
    "combat_test: every brick, question and coin group, and scene.lifecycle's count and order.",
    [patch(SCENE_C, "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());\n",
           "")],
)

mutation(
    "tile-system-before-physics",
    "`TileSystem` is registered *before* the physics step, so it reads a report the physics "
    "step has already emptied.",
    "combat_test: the brick, question and coin groups - nothing works at all, which is what "
    "makes the ordering worth pinning - and scene.lifecycle's index.\n"
    "\n"
    "The report is cleared at the top of every physics update, so this position reads an empty "
    "one rather than the previous frame's. That is not an accident of the ordering: it is why "
    "'a stale report' is unrepresentable in this engine at all.",
    [patch(SCENE_C,
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());\n\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    // MUTATION: reads a report physics has not filled\n"
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());")],
)

mutation(
    "coin-spawned-during-the-tile-walk",
    "The coin is created inside the walk over the tiles, which is the structural change "
    "[engine::ecs::Query] documents as unsupported.",
    "A genuine use-after-invalidate rather than a behavioural change, so it is expected to CRASH "
    "or to be detected by AddressSanitizer. Worth running: it proves the four-walk structure in "
    "[engine::systems::TileSystem] is load-bearing rather than stylistic, and it is the one "
    "mutation in this set whose detection is the *sanitizers* rather than an assertion.",
    [patch(TILE_C,
           "transform.position.y - kCoinSpawnHeight},\n                             coinSpawns);",
           "transform.position.y - kCoinSpawnHeight},\n                             coinSpawns);\n"
           "            spawnCoin(entities, Vec2{transform.position.x, transform.position.y - "
           "kCoinSpawnHeight}); // MUTATION: mid-walk")],
)

# ---------------------------------------------------------------------------
# The level and the assets
# ---------------------------------------------------------------------------

mutation(
    "no-brick-in-the-committed-level",
    "The committed level's two bricks are removed.",
    "combat_test: 'the committed level classifies its bricks' - which counts them through a real "
    "`PlayScene` - and 'firing from the committed level breaks a brick', which needs a target "
    "to shoot at, and level_test's counts of the shipped level.\n"
    "\n"
    "The mutation is on the **data**, not the code, which is the point: the phase's end-to-end "
    "claim is about the shipped level, and only the shipped level can break it.",
    [patch(LEVEL_TXT, "Tile mario_Brick_tile 8 1\nTile mario_Brick_tile 5 3\n", "")],
)

mutation(
    "brick-uses-the-wrong-artwork",
    "`mario_Brick_tile` is declared over the bush instead of the ground tile, so a brick is no "
    "longer a grid cell and the level's bricks are the wrong shape.",
    "level_test: 'each tile keeps its own animation name', which pins the brick's collider at "
    "64x64, and combat_test: 'a brick starts whole and solid'.\n"
    "\n"
    "An asset-configuration change, because that is where the fact lives. A brick that was the "
    "wrong size would quietly become a different kind of wall.",
    [patch(ASSETS_TXT, "Animation mario_Brick_tile           mario_ground        1  1",
           "Animation mario_Brick_tile           mario_BigBush       1  1")],
)

mutation(
    "brick-is-declared-solid",
    "`tileTypeFor` no longer recognises the brick animation, so every brick is spawned as an "
    "ordinary solid tile.",
    "combat_test: 'the committed level classifies its bricks' counts them through a real "
    "`PlayScene` and finds none, and 'a bullet hitting a brick starts the explosion' cannot "
    "find a brick at all - and level_test sees 24 tiles where it expects 26 typed.\n"
    "\n"
    "The mutation is on the *classification*, which is the one place the level's animation name "
    "becomes gameplay - so it is the one place a typo would be a silent behaviour change rather "
    "than a compile error.",
    [patch(TILE_H,
           "    if (animationName == kBrickAnimationName)\n    {\n        return TileType::Brick;\n    }",
           "    // MUTATION: a brick is just ground")],
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

    A clean `Failed` outranks a crash *somewhere else*: declaring a three-frame run strip
    makes the asset manager throw from its constructor, so every suite that builds one aborts,
    and several also fail with real assertion failures first. Reporting CRASHED alone would
    throw those detections away; reporting TEST_DETECTED alone would hide the aborts. CRASHED
    is reserved for the case where nothing managed to assert anything.
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