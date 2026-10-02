#!/usr/bin/env python3
"""Mutation testing for Phase 19, the debug toggles.

Inherits the discipline of `tools/mutate_phase18.py`, and every item in that header
still applies:

* A patch that matches nothing produces a green run and would be recorded as EQUIVALENT,
  which is the most expensive way for a mutation harness to be wrong. Every patch is
  verified to have matched the expected number of times *and* to have survived write-back.
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

Phase 19 adds four traps of its own, and each one is a mutation that a count cannot see:

* **A toggle is a flag plus a decision.** `wasPressed` and `isActive` produce the same
  picture on the frame a key goes down and differ on every frame after it, so a
  level-triggered toggle needs a group that *holds* the key rather than one that taps it.
* **One gate means no individual system can be left running.** The phase brief asks for
  mutations that "allow PlayerSystem while paused" and "allow LifetimeSystem while
  paused", and this design makes both of those unrepresentable: there is a single
  `if (m_paused) return;` and the systems are below it. The mutations that reach the same
  failures are therefore the gate being moved, deleted, or replaced by something that
  looks equivalent - freezing the clock instead of skipping the systems, which leaves
  every frame-counted system running while the integrated ones stop.
* **Render order is not a count.** Two passes drawing the same things in the other order
  submit the same number of draw calls, so the only way to see an overlay moved
  underneath the game is to record the order.
* **A guard in one of two image queries is invisible.** `RenderSystem` submits a whole
  texture in one query and an animation frame in another, and the committed level only
  ever holds animated entities. Both loops are covered by `debug.controls`, which is the
  reason the "texture guard dropped from the plain-texture query" mutation below is a
  detection rather than an equivalence.
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
    os.path.join(tempfile.gettempdir(), "phase19-mutation-results.json"))

INC = "include/engine"
SRC = "src/engine"

DEBUG_H = f"{INC}/debug/DebugRenderState.hpp"
DEBUG_SYS_H = f"{INC}/systems/DebugRenderSystem.hpp"
DEBUG_SYS_C = f"{SRC}/systems/DebugRenderSystem.cpp"
RENDER_H = f"{INC}/systems/RenderSystem.hpp"
RENDER_C = f"{SRC}/systems/RenderSystem.cpp"
PLAY_H = f"{INC}/scene/PlayScene.hpp"
PLAY_C = f"{SRC}/scene/PlayScene.cpp"
MENU_C = f"{SRC}/scene/MenuScene.cpp"

MUTATIONS = []


def mutation(name, description, intent, patches, equivalent=None):
    MUTATIONS.append({"name": name, "description": description, "intent": intent,
                      "patches": patches, "equivalent": equivalent})


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


# ---------------------------------------------------------------------------
# Pause: the toggle
# ---------------------------------------------------------------------------

mutation(
    "pause-toggle-removed",
    "The pause control is deleted, so nothing ever stops the simulation.",
    "debug.controls: every pause group. The phase's central claim is that `P` stops the "
    "world, and this is that claim with the switch taken out.",
    [patch(PLAY_C,
           "    if (actions.wasPressed(input::Action::Pause))\n    {\n        m_paused = !m_paused;\n    }\n",
           "")],
)

mutation(
    "pause-toggle-is-a-no-op",
    "The toggle assigns the flag to itself, so the `!` is lost.",
    "The same groups as above, and a different mistake from the deletion: somebody "
    "deleted one character. A deletion mutation would have been the same fact under a "
    "second name, which is how a harness ends up with two mutations and one thing to say.",
    [patch(PLAY_C, "        m_paused = !m_paused;", "        m_paused = m_paused;")],
)

mutation(
    "pause-can-never-be-lifted",
    "The toggle can only ever set the flag, so the game can be paused and never resumed.",
    "debug.controls: every group that resumes. Written as `= true` rather than as a "
    "removed `else`, because a one-way switch is the more plausible half-finished edit: "
    "the press is found, the release is not.",
    [patch(PLAY_C, "        m_paused = !m_paused;", "        m_paused = true;")],
)

mutation(
    "pause-is-level-triggered",
    "The pause action is read as held rather than as a press edge.",
    "debug.controls: *holding P does not toggle pause every frame*, which holds the key "
    "for three seconds of frames and asserts the flag on every one of them, and every "
    "group that resumes.\n"
    "\n"
    "The mistake the course's own sentence warns about for the jump key, wearing a pause "
    "costume: a level-triggered toggle flips sixty times a second, so the game appears to "
    "flicker rather than to stop. A group that only ever tapped `P` would pass this.",
    [patch(PLAY_C,
           "    if (actions.wasPressed(input::Action::Pause))\n    {\n        m_paused = !m_paused;\n    }",
           "    if (actions.isActive(input::Action::Pause)) // MUTATION: level, not edge\n    {\n"
           "        m_paused = !m_paused;\n    }")],
)

# ---------------------------------------------------------------------------
# Pause: the gate
# ---------------------------------------------------------------------------

mutation(
    "pause-gate-removed",
    "The pause gate is deleted, so every system runs in a paused frame.",
    "debug.controls: every pause group - the player walks on, the bullet flies on, the "
    "lifetime counts down and the explosion finishes. The one-liner the whole design "
    "exists to make unnecessary.",
    [patch(PLAY_C, "    if (m_paused)\n    {\n        return;\n    }\n\n    m_systems.update(",
           "    m_systems.update(")],
)

mutation(
    "pause-frozen-by-zeroing-the-clock",
    "The gate is replaced by freezing time: the systems still run, with a zero delta.",
    "debug.controls: *a bullet stops and keeps its lifetime and then resumes* and *an "
    "explosion stops with the world and finishes when it resumes*.\n"
    "\n"
    "This is the mutation the phase brief describes as *'allow PhysicsSystem while paused'* "
    "and *'allow LifetimeSystem while paused'* at once, and it is worth the two patches it "
    "costs. Zeroing the delta looks like freezing the world and is the more plausible way "
    "to write it, because the frame time is what 'time passing' means to most people.\n"
    "\n"
    "It is only half a freeze. `PhysicsSystem` integrates `velocity * deltaSeconds`, so the "
    "player and the bullet do stop. `LifetimeSystem` and `AnimationSystem` count **game "
    "frames**, not seconds, so a hundred and nine frames of a bullet's life evaporate while "
    "the player watches and the explosion plays out behind the pause. Exactly the failure "
    "the brief warns about, and one that a player looking only at the player would never "
    "see.",
    [patch(PLAY_C, "    if (m_paused)\n    {\n        return;\n    }\n\n    m_systems.update(",
           "    // MUTATION: freeze the clock rather than skip the systems\n"),
     patch(PLAY_C, "    m_systems.update(m_world, actions, deltaSeconds);",
           "    m_systems.update(m_world, actions, m_paused ? 0.0F : deltaSeconds);")],
)

mutation(
    "pause-gate-above-the-toggles",
    "The gate moves above the debug controls, so the controls only work while the game runs.",
    "debug.controls: *a debug control still works in a paused game*, which presses `C`, `G` "
    "and `T` in a frozen frame and asserts what the renderer received.\n"
    "\n"
    "A pause that also freezes the debugger is not a pause anybody can use: the whole reason "
    "to stop is to look at something. It is also a plausible edit, because the gate looks "
    "like it belongs at the top of the frame.",
    [patch(PLAY_C,
           "    debug::DebugRenderState& debug = m_renderSystem.debugRenderState();",
           "    // MUTATION: the gate is above the controls now\n"
           "    if (m_paused)\n    {\n        return;\n    }\n\n"
           "    debug::DebugRenderState& debug = m_renderSystem.debugRenderState();"),
     patch(PLAY_C, "    if (m_paused)\n    {\n        return;\n    }\n\n    m_systems.update(", "    m_systems.update(")],
)

mutation(
    "pause-gate-below-escape",
    "The gate moves below the Escape check, so a paused level cannot be left.",
    "debug.controls: *escape works in a paused level*.\n"
    "\n"
    "The same edit as the one above, in the other direction, and it is here because the two "
    "are independent mistakes: the gate has to be below the debug controls and below the "
    "transition request, and there is no single ordering that satisfies one and breaks the "
    "other by accident.",
    [patch(PLAY_C,
           "    if (actions.wasPressed(input::Action::Quit))\n    {\n        requestTransition(SceneTransition::to(SceneId::Menu));\n    }",
           "    // MUTATION: the gate is below Escape now\n"
           "    if (m_paused)\n    {\n        return;\n    }\n\n"
           "    if (actions.wasPressed(input::Action::Quit))\n    {\n"
           "        requestTransition(SceneTransition::to(SceneId::Menu));\n    }"),
     patch(PLAY_C, "    if (m_paused)\n    {\n        return;\n    }\n\n    m_systems.update(", "    m_systems.update(")],
)

mutation(
    "paused-state-is-static",
    "The pause flag becomes a static member, so every level in the process shares one.",
    "debug.controls: *pause is per scene and not shared*, which builds two real "
    "`PlayScene` objects, pauses one and asserts the other does not move; and *a new level "
    "starts unpaused with the game's own look*, which pauses a level and then builds a "
    "second one.\n"
    "\n"
    "The phase brief calls this one out by name, and it is the reason the source check "
    "*the debug layer is three bools and no static storage* exists at all. Every behavioural "
    "group in the file would still pass with a shared flag, because there is only ever one "
    "paused level at a time in the game.",
    [patch(PLAY_H, "    bool m_paused = false;", "    static bool m_paused; // MUTATION: shared"),
     patch(PLAY_C, "namespace engine::scene\n{\n", "namespace engine::scene\n{\n\nbool PlayScene::m_paused = false;\n")],
)

# ---------------------------------------------------------------------------
# The texture toggle
# ---------------------------------------------------------------------------

mutation(
    "textures-cannot-be-restored",
    "The `T` key clears the flag instead of toggling it, so textures never come back.",
    "debug.controls: *T removes every texture draw and puts them back*, which presses `T` "
    "twice and requires the second press to restore every draw. The reversible half of "
    "'toggles', which is the half a one-way switch gets right.",
    [patch(PLAY_C, "        debug.showTextures = !debug.showTextures;",
           "        debug.showTextures = false; // MUTATION: one way")],
)

mutation(
    "textures-can-never-be-turned-off",
    "The `T` key sets the flag instead of toggling it, so the game is drawn without artwork "
    "from the first press onwards.",
    "debug.controls: *T removes every texture draw and puts them back* and *holding T does "
    "not flicker the textures*.",
    [patch(PLAY_C, "        debug.showTextures = !debug.showTextures;",
           "        debug.showTextures = true; // MUTATION: one way the other way")],
)

mutation(
    "texture-toggle-drives-the-wrong-flag",
    "`T` toggles the bounding-box flag rather than the texture flag.",
    "debug.controls: both toggles' groups. The realistic form of the mistake is a copy "
    "and paste of the block below it, which is why the three blocks sit next to each "
    "other and why this is a two-line edit rather than a rename.",
    [patch(PLAY_C, "        debug.showTextures = !debug.showTextures;",
           "        debug.showBoundingBoxes = !debug.showBoundingBoxes; // MUTATION: wrong flag")],
)

mutation(
    "texture-flag-guard-inverted",
    "The render system's check on the flag is inverted, so textures are drawn only while "
    "the flag says they are hidden.",
    "debug.controls: every texture group, plus `render.foundation` and `animation.playback` - "
    "which build render systems of their own with the default state and would find every "
    "sprite missing.",
    [patch(RENDER_C, "        if (!m_debug.showTextures)\n        {\n            continue;\n        }",
           "        if (m_debug.showTextures) // MUTATION: inverted\n        {\n            continue;\n        }", 2)],
)

mutation(
    "texture-guard-dropped-from-the-animation-query",
    "The animation-frame query stops consulting the flag, so `T` hides a plain sprite and "
    "leaves every animated entity visible.",
    "debug.controls: *T removes every texture draw and puts them back* and *T hides a plain "
    "texture as well as an animation frame*.\n"
    "\n"
    "The two image queries are separate loops, so the flag is read twice, and half of the "
    "level's entities - the player, every bullet, every exploding brick - are drawn by the "
    "second one. A suite that only ever put animated entities in the world would not have "
    "noticed; `debug.controls` now puts one entity of each kind in the same frame.",
    [patch(RENDER_C,
           "        // Two steps, and deliberately so. The animation says which image and how\n"
           "        // it is divided; the texture it names is a separate asset, looked up by\n"
           "        // name. Resolving the texture once per frame per entity keeps the\n"
           "        // alternative - an animation carrying a texture handle - out of the\n"
           "        // component, where a copy of the artwork would exist for every entity.\n",
           "        // MUTATION: no flag check here\n"
           "        // Two steps, and deliberately so. The animation says which image and how\n"
           "        // it is divided; the texture it names is a separate asset, looked up by\n"
           "        // name. Resolving the texture once per frame per entity keeps the\n"
           "        // alternative - an animation carrying a texture handle - out of the\n"
           "        // component, where a copy of the artwork would exist for every entity.\n")],
)

mutation(
    "texture-guard-dropped-from-the-plain-texture-query",
    "The plain-texture query stops consulting the flag.",
    "debug.controls: *T hides a plain texture as well as an animation frame*, which puts one "
    "entity of each kind in the same frame and requires the whole-texture draw to disappear "
    "as well.\n"
    "\n"
    "Included specifically because it is the mutation that would have been an EQUIVALENT "
    "before that group existed: the committed level carries no `components::Texture` at all, "
    "so without an entity of that kind this guard is unreachable and no assertion in the "
    "repository can see it.",
    [patch(RENDER_C,
           "        if (!m_debug.showTextures)\n        {\n            continue;\n        }\n\n        // Same conversion as the rectangle above",
           "        // MUTATION: no flag check on the plain-texture path\n\n"
           "        // Same conversion as the rectangle above")],
)

# ---------------------------------------------------------------------------
# The bounding-box toggle
# ---------------------------------------------------------------------------

mutation(
    "bounds-cannot-be-turned-off",
    "The `C` key sets the flag instead of toggling it, so the boxes appear on the first "
    "press and stay for ever.",
    "debug.controls: *C draws one box per collider at the collider's own size*, which "
    "requires an empty frame before and an empty frame after.",
    [patch(PLAY_C, "        debug.showBoundingBoxes = !debug.showBoundingBoxes;",
           "        debug.showBoundingBoxes = true; // MUTATION: one way")],
)

mutation(
    "bounds-always-drawn",
    "The bounding-box pass ignores the flag and always draws.",
    "debug.controls: every group that asserts an empty frame - the texture group's first "
    "render, the grid group's first render, the reset group, and *C draws one box per "
    "collider*'s own off-state.",
    [patch(DEBUG_SYS_C, "    if (m_state.showBoundingBoxes)\n    {\n        drawBoundingBoxes(entities);\n    }",
           "    drawBoundingBoxes(entities); // MUTATION: always")],
)

mutation(
    "bounds-never-drawn",
    "The bounding-box pass never draws, whatever the flag says.",
    "debug.controls: *C draws one box per collider at the collider's own size*, *an entity "
    "with no collider gets no box* (which counts boxes and would find none), *a destroyed "
    "brick's box disappears with its collider*, and *a debug control still works in a paused "
    "game*.",
    [patch(DEBUG_SYS_C, "    if (m_state.showBoundingBoxes)\n    {\n        drawBoundingBoxes(entities);\n    }",
           "    // MUTATION: never drawn")],
)

mutation(
    "bounds-for-everything",
    "A box is drawn for every entity with a transform, at a hard-coded cell when the entity "
    "has no collider.",
    "debug.controls: *an entity with no collider gets no box*, which adds an entity that is "
    "animated and not collidable and requires the box count to equal the collider count.\n"
    "\n"
    "This is the mutation the phase brief names - *'render bounds for entities without "
    "Collider'* - and it is the one that would make the overlay lie about the world's "
    "geometry: a box the physics never used, at a size nothing chose.",
    [patch(DEBUG_SYS_C,
           "    for (auto&& [entity, transform, collider] : entities.query<components::Transform, components::Collider>())\n"
           "    {\n        static_cast<void>(entity);",
           "    for (auto&& [entity, transform] : entities.query<components::Transform>())\n    {\n"
           "        const components::Collider* const box = entity.tryGetConstComponent<components::Collider>();\n"
           "        // MUTATION: a box for everything, at a made-up size when there is no collider\n"
           "        const Vec2 size = box != nullptr ? box->size : Vec2{64.0F, 64.0F};\n"
           "        m_renderer->drawRectangle(size, kBoundingBoxColor,\n"
           "                                  graphics::toRenderTransform(transform, *m_camera));\n        continue;\n    }\n\n"
           "    for (auto&& [entity, transform, collider] : entities.query<components::Transform, components::Collider>()))\n"
           "    {\n        static_cast<void>(entity);")],
)

mutation(
    "bounds-use-a-constant-cell",
    "The box is the grid's 64 pixels rather than the collider's own size.",
    "debug.controls: *C draws one box per collider at the collider's own size*, which "
    "requires the level's 70x70 pipe and its 40x60 player to appear at their own sizes and "
    "not at a cell.\n"
    "\n"
    "The phase brief asks for this explicitly - *'Do not hard-code 64x64'* - and it is the "
    "mistake a debug overlay is most likely to make, because the grid is 64 and it is right "
    "there in the same file.",
    [patch(DEBUG_SYS_C,
           "        m_renderer->drawRectangle(collider.size, kBoundingBoxColor, graphics::toRenderTransform(transform, *m_camera));",
           "        // MUTATION: a hard-coded cell rather than the collider's own size\n"
           "        static_cast<void>(collider);\n"
           "        m_renderer->drawRectangle(Vec2{64.0F, 64.0F}, kBoundingBoxColor,\n"
           "                                  graphics::toRenderTransform(transform, *m_camera));")],
)

# ---------------------------------------------------------------------------
# The grid toggle
# ---------------------------------------------------------------------------

mutation(
    "grid-cannot-be-turned-off",
    "The `G` key sets the flag instead of toggling it.",
    "debug.controls: *G draws the level grid snapped to cell boundaries*, which requires the "
    "grid to be gone again on the second press.",
    [patch(PLAY_C, "        debug.showGrid = !debug.showGrid;", "        debug.showGrid = true; // MUTATION: one way")],
)

mutation(
    "grid-always-drawn",
    "The grid pass ignores the flag and always draws.",
    "debug.controls: every group that asserts an empty rectangle list - the texture group's "
    "first render, the bounds group's off state, the reset group and the new-level group.",
    [patch(DEBUG_SYS_C, "    if (m_state.showGrid)\n    {\n        drawGrid();\n    }",
           "    drawGrid(); // MUTATION: always")],
)

mutation(
    "grid-never-drawn",
    "The grid pass never draws, whatever the flag says.",
    "debug.controls: the three grid groups and *a debug control still works in a paused "
    "game*, which counts twenty-one vertical lines and twelve horizontal ones.",
    [patch(DEBUG_SYS_C, "    if (m_state.showGrid)\n    {\n        drawGrid();\n    }",
           "    // MUTATION: never drawn")],
)

mutation(
    "grid-uses-the-wrong-cell-size",
    "The grid is drawn on 32-pixel cells rather than the course's 64.",
    "debug.controls: *G draws the level grid snapped to cell boundaries*, which requires "
    "twenty-one vertical lines at zoom 1 and every line's world position to be an exact "
    "multiple of 64; and the other two grid groups.\n"
    "\n"
    "A finer grid is a plausible mistake rather than an arbitrary one - a developer who "
    "wanted a denser picture - and it is invisible to a group that only checked that *some* "
    "lines were drawn.",
    [patch(DEBUG_SYS_C, "    const float cell = level::LevelGrid::kCellSize;",
           "    const float cell = level::LevelGrid::kCellSize * 0.5F; // MUTATION: a finer grid")],
)

mutation(
    "grid-is-screen-space",
    "The grid lines are submitted in screen pixels, so the camera does not apply to them.",
    "debug.controls: *the grid follows the camera*, which reads the world position of a "
    "known line back through `screenToWorld` in two views; and *the grid scales with the "
    "zoom*, which requires the line spacing to double.\n"
    "\n"
    "The phase brief asks for this - *'Do not make them screen-space merely because "
    "screen-space support exists'* - and it is the mistake that looks **right**: the lines "
    "still land on the cell edges at the default camera position and zoom, and only the "
    "movement and the magnification reveal it. The world-position assertions are what see "
    "it, and they exist because the picture at one camera position cannot.",
    [patch(DEBUG_SYS_C,
           "    renderer.drawRectangle(size, color,\n"
           "                           graphics::toRenderTransform(components::Transform{centre, Vec2{0.0F, 0.0F},\n"
           "                                                                              Vec2{1.0F, 1.0F}, 0.0F},\n"
           "                                                       camera));",
           "    // MUTATION: screen space, so the camera does not apply\n"
           "    static_cast<void>(camera);\n"
           "    renderer.drawRectangle(size, color,\n"
           "                           graphics::toScreenTransform(components::Transform{centre, Vec2{0.0F, 0.0F},\n"
           "                                                                            Vec2{1.0F, 1.0F}, 0.0F}));\n"
           "    (void)0;")],
)

mutation(
    "grid-line-thickness-changed",
    "A line is two world pixels thick rather than one.",
    "debug.controls: *G draws the level grid snapped to cell boundaries*, which reads the "
    "two orientations apart by their thickness and so finds neither, and *the grid scales "
    "with the zoom*.\n"
    "\n"
    "A cosmetic change with a real consequence: the recorder tells a line from a box by its "
    "thickness, and at two pixels a reader of this suite can no longer tell a grid line "
    "from a collider box of that size - which is exactly the ambiguity a thinner line "
    "avoids.",
    [patch(DEBUG_SYS_C, "constexpr float kGridLineThickness = 1.0F;",
           "constexpr float kGridLineThickness = 2.0F; // MUTATION: thicker")],
)

mutation(
    "grid-lines-do-not-reach-the-edge-of-the-view",
    "Each line stops exactly at the visible rectangle, with no cell of overlap.",
    "debug.controls: *G draws the level grid snapped to cell boundaries*, which requires "
    "each line to span the viewport plus one cell at each end.\n"
    "\n"
    "Off by two cells in a world-space overlay, which a screenshot at a default camera "
    "position would show as a grid whose outer ring is missing - easy to mistake for the "
    "grid simply ending at the world edge.",
    [patch(DEBUG_SYS_C,
           "    const Vec2 span{bottomRight.x - topLeft.x + (cell * 2.0F), bottomRight.y - topLeft.y + (cell * 2.0F)};",
           "    // MUTATION: no overlap past the visible rectangle\n"
           "    const Vec2 span{bottomRight.x - topLeft.x, bottomRight.y - topLeft.y};")],
)

# ---------------------------------------------------------------------------
# Escape
# ---------------------------------------------------------------------------

mutation(
    "play-to-menu-removed",
    "A level no longer asks for the menu on Escape.",
    "scene.lifecycle: *the play scene goes back to the menu*, which has existed since Phase "
    "15 and asserts the requested id. debug.controls: *escape in a level asks for the menu* "
    "and *escape works in a paused level*.",
    [patch(PLAY_C,
           "    if (actions.wasPressed(input::Action::Quit))\n    {\n        requestTransition(SceneTransition::to(SceneId::Menu));\n    }",
           "    // MUTATION: Escape does nothing in a level")],
)

mutation(
    "play-asks-for-a-different-scene",
    "A level asks to be replaced by another level rather than by the menu.",
    "scene.lifecycle's transition group and debug.controls' two escape groups. The "
    "plausible mistake is a copy of the menu's own `to(SceneId::Play)` in the wrong file.",
    [patch(PLAY_C, "requestTransition(SceneTransition::to(SceneId::Menu));",
           "requestTransition(SceneTransition::to(SceneId::Play)); // MUTATION")],
)

mutation(
    "play-bypasses-the-transition",
    "A level asks the application to quit instead of asking for a scene.",
    "scene.lifecycle: *the play scene goes back to the menu*, which requires a scene id; "
    "debug.controls: *escape in a level asks for the menu*, which requires "
    "`quits()` to be false as well.\n"
    "\n"
    "Both answers come from the same value and one of them is a whole other meaning - "
    "'leave the game' rather than 'go back'. Asking for the wrong one is invisible to a "
    "group that only checked that *something* was requested.",
    [patch(PLAY_C, "        requestTransition(SceneTransition::to(SceneId::Menu));",
           "        requestTransition(SceneTransition::quitApplication()); // MUTATION: quits instead")],
)

mutation(
    "menu-quit-removed",
    "The main menu no longer acts on Escape.",
    "debug.controls: *escape on the main menu asks the application to quit*.\n"
    "\n"
    "The other half of the course's sentence, and the one behaviour this phase adds to "
    "`MenuScene`. A menu that ignored Escape was reachable and could be left only with the "
    "mouse close button or a keyboard shortcut nobody is told about.",
    [patch(MENU_C,
           "    if (actions.wasPressed(input::Action::Quit))\n    {\n        requestTransition(SceneTransition::quitApplication());\n    }\n",
           "")],
)

mutation(
    "menu-asks-for-a-scene-instead-of-quitting",
    "The menu asks for the play scene on Escape rather than asking to quit.",
    "debug.controls: *escape on the main menu asks the application to quit*, which requires "
    "`quits()` to be true **and** `scene()` to be empty.",
    [patch(MENU_C, "        requestTransition(SceneTransition::quitApplication());\n    }\n\n    refreshSelection();",
           "        requestTransition(SceneTransition::to(SceneId::Play)); // MUTATION\n    }\n\n    refreshSelection();")],
)

# ---------------------------------------------------------------------------
# Ownership and the frame
# ---------------------------------------------------------------------------

mutation(
    "debug-flags-are-static",
    "The three rendering flags become one process-wide value behind an accessor.",
    "debug.controls: *the debug state is not shared between two scenes*, which renders both "
    "and requires one to have boxes and the other none; *a new level starts unpaused with "
    "the game's own look*; and the source check *the debug layer is three bools and no "
    "static storage*, which reads the header.\n"
    "\n"
    "The phase brief asks for this mutation by name - *'leak debug flags if they are "
    "supposed to reset'* - and it is worth three patches because the mistake has three "
    "reachable forms: a member moved to a static, an accessor returning a static, and a "
    "file-scope variable. Only the second keeps the code compiling, which is itself worth "
    "recording: 'no static state' is only enforceable by reading, never by compiling.",
    [patch(RENDER_H,
           "    [[nodiscard]] debug::DebugRenderState& debugRenderState() noexcept { return m_debug; }\n"
           "    [[nodiscard]] const debug::DebugRenderState& debugRenderState() const noexcept { return m_debug; }",
           "    /// MUTATION: one process-wide set of flags\n"
           "    [[nodiscard]] static debug::DebugRenderState& sharedDebug()\n"
           "    {\n        static debug::DebugRenderState state{};\n        return state;\n    }\n\n"
           "    [[nodiscard]] debug::DebugRenderState& debugRenderState() noexcept { return sharedDebug(); }\n"
           "    [[nodiscard]] const debug::DebugRenderState& debugRenderState() const noexcept { return sharedDebug(); }"),
     patch(RENDER_H, "    debug::DebugRenderState m_debug{};", ""),
     patch(RENDER_C, "if (!m_debug.showTextures)", "if (!debugRenderState().showTextures)", 2)],
)

mutation(
    "overlay-pass-registered-as-a-simulation-system",
    "The overlay pass is registered in the system manager as well as being driven by the "
    "render pass.",
    "debug.controls: *the debug render pass is not a simulation system*, which counts nine "
    "systems and finds ten, and which reads the scene's source for the registration. Every "
    "grid group also fails, because the grid would be submitted during the update and the "
    "recording renderer's next `beginFrame` would clear it before anything looked.",
    [patch(PLAY_C,
           "    m_systems.add<engine::systems::AnimationSystem>(context.assets());",
           "    m_systems.add<engine::systems::AnimationSystem>(context.assets());\n"
           "    // MUTATION: a render pass in the simulation list\n"
           "    m_systems.add<engine::systems::DebugRenderSystem>(context.renderer(), context.camera(),\n"
           "                                                         m_renderSystem.debugRenderState());")],
)

mutation(
    "overlay-pass-drawn-before-the-game",
    "The two render passes run in the other order, so the overlays go underneath the game.",
    "debug.controls: *the overlay pass is drawn on top of the game*, which reads the order "
    "of the draw calls.\n"
    "\n"
    "This mutation is here because of what it is *not* caught by. Every other assertion in "
    "the file is a count or a position, and both are identical whichever order the passes "
    "run in. A bounding box underneath the sprite it belongs to is a box nobody can see, and "
    "the suite would have been green.",
    [patch(PLAY_C,
           "    m_renderSystem.update(m_world, m_noActions, 0.0F);\n    m_debugRenderSystem.update(m_world, m_noActions, 0.0F);",
           "    // MUTATION: the overlay is drawn first\n"
           "    m_debugRenderSystem.update(m_world, m_noActions, 0.0F);\n"
           "    m_renderSystem.update(m_world, m_noActions, 0.0F);")],
)

mutation(
    "the-three-toggles-share-one-flag",
    "All three render flags are the same member, so one key changes all three at once.",
    "debug.controls: *the debug state is not shared between two scenes* and *a new level "
    "starts unpaused with the game's own look*, both of which read the three flags "
    "independently; and every grid or bounds group, which would find the other overlay "
    "switched on as well.\n"
    "\n"
    "A one-character change - a copy of the same member name typed into two assignments - "
    "and the reason the flags are three named fields rather than one index.",
    [patch(PLAY_C, "        debug.showBoundingBoxes = !debug.showBoundingBoxes;",
           "        debug.showTextures = !debug.showTextures; // MUTATION: same flag as T")],
)

mutation(
    "the-grid-flag-is-read-from-the-box-flag",
    "The grid pass is drawn whenever the bounding-box flag is set.",
    "debug.controls: every grid group - `G` alone draws no grid at all, and `C` alone draws "
    "one.",
    [patch(PLAY_C, "        debug.showGrid = !debug.showGrid;",
           "        debug.showBoundingBoxes = !debug.showBoundingBoxes; // MUTATION: shares the boxes' flag")],
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
    result = subprocess.run(["ctest", "--output-on-failure", "--timeout", "600"],
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

    A clean `Failed` outranks a crash *somewhere else*: declaring a two-argument
    constructor on a class whose code calls it makes every suite that builds one abort, and
    several also fail with real assertion failures first. Reporting CRASHED alone would
    throw those detections away; reporting TEST_DETECTED alone would hide the aborts.
    CRASHED is reserved for the case where nothing managed to assert anything.
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
                detail += "\n    " + json.dumps(failing_checks(test_output))[:1200]
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