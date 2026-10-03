#!/usr/bin/env python3
"""Mutation testing for Phase 20, the A3 end-to-end slice.

Inherits the discipline of `tools/mutate_phase19.py`, and every item in that header
still applies: patches are verified to have matched and to have survived write-back,
reverts go through `git checkout`, the tree is rebuilt and re-verified green between
mutations, a crash is not a detection, the suite list is compared, and a proven
equivalence has to say why.

This phase is **integration**, so almost every mutation here is about something that is
invisible in a single system and fatal in a game:

* **Order is invisible to a count.** Nine systems in the wrong order still report nine
  systems, and most of the wrong orders still pass most groups. The ones that matter -
  `ShootSystem` after the integration, `TileSystem` before physics, `PlayerStateSystem`
  before physics - are all here.
* **A frame of lag is invisible unless something is compared across frames.** A player
  that moved a frame late lands in exactly the same place, and only the bullet's
  *first* frame of travel shows it.
* **The level is data.** A mutation on `level1.txt` or on `assets.txt` is the only kind
  that can break "the shipped level is playable", which is the phase's whole claim.

Two things this harness cannot judge, and says so rather than pretending otherwise:

* **The windowed pixel path.** `gameplay.a3_vertical_slice` uses a recording renderer,
  so a mutation that broke only the SFML boundary would not be seen here. It would be
  seen by `application.scripted_playthrough`, which runs the shipped binary with the
  script and checks its exit code, and by `render.foundation`'s real-pixel groups.
* **The recorded input script.** A mutation that broke `--keys` would be caught by the
  same `application.scripted_playthrough` ctest, which runs it as part of every mutation
  because the harness runs `ctest` over the whole suite list.
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
    os.path.join(tempfile.gettempdir(), "phase20-mutation-results.json"))

INC = "include/engine"
SRC = "src/engine"

PLAY_C = f"{SRC}/scene/PlayScene.cpp"
PLAY_H = f"{INC}/scene/PlayScene.hpp"
MENU_C = f"{SRC}/scene/MenuScene.cpp"
APP_C = f"{SRC}/Application.cpp"
SHOOT_C = f"{SRC}/systems/ShootSystem.cpp"
TILE_C = f"{SRC}/systems/TileSystem.cpp"
LEVEL_TXT = "assets/levels/level1.txt"

MUTATIONS = []


def mutation(name, description, intent, patches, equivalent=None):
    MUTATIONS.append({"name": name, "description": description, "intent": intent,
                      "patches": patches, "equivalent": equivalent})


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


# ---------------------------------------------------------------------------
# The recorded input script - the seam this phase added
# ---------------------------------------------------------------------------

mutation(
    "recorded-hold-is-one-frame",
    "A `KEY:120` token holds the key for a single frame instead of a hundred and twenty.",
    "application.scripted_playthrough, which runs the shipped binary with `--keys` and "
    "checks its exit code, and `gameplay.a3_vertical_slice` is unaffected because it "
    "drives keys directly.\n"
    "\n"
    "The mutation that would be invisible to any behavioural group, and the reason the "
    "scripted ctest exists at all: a hold that is really a tap still runs the whole "
    "script and still exits zero, so nothing about the program's *behaviour* would "
    "change - only whether the recorded sequence meant what it said.",
    # The comment goes on its own line rather than at the end of this one, because the
    # statement's semicolon is on this line and a trailing comment would eat it. That is
    # the whole reason the first version of this mutation was BUILD_REJECTED.
    [patch(APP_C,
           "        const std::string next = std::string{name} + \":\" + std::to_string(count - 1U);",
           "        // MUTATION: a hold that is really a single frame\n"
           "        const std::string next = std::string{name} + \":\" + std::to_string(0U);")],
)

mutation(
    "recorded-key-is-never-pressed",
    "The script reads tokens but never presses anything, so `--keys` is accepted and "
    "silently does nothing.",
    "application.scripted_playthrough: the game runs to its frame limit and exits zero, "
    "so this is caught only because the ctest also asserts on stderr... which it does "
    "not.\n"
    "\n"
    "That sentence is the honest limit of this mutation, and it is why it is here: "
    "`--keys` that does nothing is indistinguishable from `--keys` that works, because "
    "both end in a clean exit. Reported as UNDETECTED with the reason recorded, rather "
    "than quietly dropped - a mutation harness that only lists its successes cannot be "
    "read.",
    [patch(APP_C,
           "    const input::Key key = toKey(name);",
           "    const input::Key key = input::Key::Unknown; // MUTATION: parsed and dropped")],
)

mutation(
    "recorded-keys-are-applied-before-events",
    "The recorded script is fed to `Input` before `processEvents()` rather than after.",
    "PROVEN EQUIVALENT for every run this harness can make, and the proof is the whole "
    "point of recording it.\n"
    "\n"
    "`Application::processEvents` forwards real window events into the same `Input`. In "
    "every run this harness performs, the window receives **no keyboard events at all** - "
    "there is nobody at the keyboard, and the only input in the frame is the script's - "
    "so `processEvents` contributes nothing to `m_input` and the two orders produce "
    "byte-identical input state for every frame.\n"
    "\n"
    "It is **not** equivalent in general, and the reason it is here rather than omitted "
    "is that difference: with a key physically held, feeding the script first lets the "
    "real `processKeyUp` clear it, while feeding it second lets the script's own "
    "press-release win. No automated environment can tell those apart, so the production "
    "order is written down in `Application.hpp` rather than left to a test that could not "
    "see it either way.",
    [patch(APP_C,
           "        processEvents();\n\n        if (!m_isRunning)\n        {\n            break; // the window was closed while we were handling events\n        }\n\n        // Where a frame's keyboard comes from. Empty in every ordinary run, and the\n        // one branch is the whole cost.\n        applyRecordedInput();",
           "        // MUTATION: the script is fed before the real events\n        applyRecordedInput();\n\n        processEvents();\n\n        if (!m_isRunning)\n        {\n            break; // the window was closed while we were handling events\n        }")],
)

# ---------------------------------------------------------------------------
# System order - the phase's central failure mode
# ---------------------------------------------------------------------------

mutation(
    "shoot-system-after-physics",
    "`ShootSystem` is registered after the physics step, so a bullet waits a frame for "
    "its first move.",
    "gameplay.a3_vertical_slice: *shooting spawns a bullet that travels and dies* asserts "
    "the bullet's position on the frame it was fired, and *a bullet destroys the committed "
    "level's brick* has to wait a frame longer for a collision that cannot happen without "
    "the move.\n"
    "\n"
    "The neighbouring position, and the one a person reaches for while reordering a list, "
    "which is why it is here rather than something more dramatic.",
    [patch(PLAY_C, "    m_systems.add<engine::systems::ShootSystem>(context.assets());\n", ""),
     patch(PLAY_C,
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    // MUTATION: the shot is created after the integration\n"
           "    m_systems.add<engine::systems::ShootSystem>(context.assets());\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});")],
)

mutation(
    "tile-system-before-physics",
    "`TileSystem` is registered before the physics step, so it reads a report physics has "
    "not filled.",
    "gameplay.a3_vertical_slice: every brick group. Nothing about a brick works at all, "
    "which is what makes the ordering worth pinning rather than merely tidy.",
    [patch(PLAY_C,
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());\n\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    // MUTATION: reads a report physics has not filled\n"
           "    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());")],
)

mutation(
    "player-state-system-not-registered",
    "`PlayerStateSystem` is not registered, so the player's `grounded` flag and state are "
    "whatever they were at spawn.",
    "gameplay.a3_vertical_slice: *the player jumps and lands on the same floor* - the jump "
    "gate reads `grounded`, which is still the spawn value - and *falling out of the world "
    "respawns the player*, and scene.lifecycle's system count and order.\n"
    "\n"
    "It began as `player-state-system-before-physics`, which is the mutation the phase "
    "brief asks for by name, and it is unrepresentable rather than merely hard: the system "
    "reads the report *physics* publishes, so registering it first means either a report "
    "that does not exist yet or a second physics system. Which is the finding: with one "
    "report, \"before physics\" is not a mistake this engine can make. Removing it is the "
    "next-closest real one.",
    [patch(PLAY_C, "    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());\n", "")],
)

mutation(
    "lifetime-after-the-simulation",
    "`LifetimeSystem` moves to after the tile system, so a coin is counted down a frame "
    "after it is created and a bullet expires a frame late.",
    "combat.tiles: the two coin lifetime groups and the two bullet lifetime groups, all "
    "of which step to the last frame of an entity's life and require it to still be there.",
    [patch(PLAY_C, "    m_systems.add<engine::systems::LifetimeSystem>();\n", ""),
     patch(PLAY_C,
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});",
           "    // MUTATION: the countdown now runs after everything that creates one\n"
           "    m_systems.add<engine::systems::LifetimeSystem>();\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});")],
)

mutation(
    "camera-before-physics",
    "`CameraSystem` moves before the physics step, so the camera follows where the player "
    "started the frame rather than where they ended it.",
    "gameplay.a3_vertical_slice: *the camera follows the player in both axes* and *the whole "
    "chain in one pass*, both of which compare the camera's position with the player's "
    "position on the same frame.\n"
    "\n"
    "A frame of camera lag is one that no single-system test can see, because there is no "
    "single system here - it only exists when the two run in this order.",
    [patch(PLAY_C,
           "    const engine::systems::PhysicsSystem& physics = m_systems.add<engine::systems::PhysicsSystem>();",
           "    // MUTATION: the camera follows the start of the frame\n"
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    const engine::systems::PhysicsSystem& physics = m_systems.add<engine::systems::PhysicsSystem>();"),
     patch(PLAY_C,
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    // Local class, not an engine one - see its own documentation above.\n",
           "    // Local class, not an engine one - see its own documentation above.\n")],
)

mutation(
    "animation-system-first",
    "`AnimationSystem` moves to the front, so the animation chosen this frame is advanced "
    "before the frame that chose it.",
    "animation.playback and player.behaviour, whose groups assert which frame is showing "
    "after a state change, and combat.tiles' explosion-lifecycle group.",
    [patch(PLAY_C,
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n",
           "    // MUTATION: the frame is advanced before the state that chose it\n"
           "    m_systems.add<engine::systems::AnimationSystem>(context.assets());\n"
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n"),
     patch(PLAY_C, "    m_systems.add<engine::systems::AnimationSystem>(context.assets());\n\n    // Read, parse, spawn",
           "\n    // Read, parse, spawn")],
)

mutation(
    "player-system-after-physics",
    "`PlayerSystem` moves after the physics step, so the velocity it writes is the "
    "velocity physics already integrated.",
    "gameplay.a3_vertical_slice: *the player walks at the level's own speed*, which "
    "compares thirty frames of travel with the level's 200 px/s - a frame of lag makes "
    "the first frame of the walk produce nothing at all.",
    [patch(PLAY_C, "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n", ""),
     patch(PLAY_C,
           "    m_systems.add<ZoomKeysSystem>(context.camera());",
           "    // MUTATION: intent is read after it has been integrated\n"
           "    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);\n"
           "    m_systems.add<ZoomKeysSystem>(context.camera());")],
)

# ---------------------------------------------------------------------------
# The orchestration boundary this phase relies on
# ---------------------------------------------------------------------------

mutation(
    "play-scene-stops-simulating",
    "`PlayScene::onUpdate` never runs its systems.",
    "Every group in `gameplay.a3_vertical_slice` except the two that only read the world "
    "at construction, and every group in `scene.lifecycle` and `player.behaviour`.\n"
    "\n"
    "The crudest possible integration break, and it is here because it is the one a "
    "half-finished refactor produces: a gate that returns before the system list.",
    [patch(PLAY_C,
           "    m_systems.update(m_world, actions, deltaSeconds);\n\n    // Deferred destruction cleanup",
           "    // MUTATION: the world never simulates\n    static_cast<void>(deltaSeconds);\n\n"
           "    // Deferred destruction cleanup")],
)

mutation(
    "world-is-never-flushed",
    "`PlayScene::onUpdate` never runs the deferred-destruction flush.",
    "scene.lifecycle's *the play scene flushes its own deferred destruction*, and "
    "gameplay.a3_vertical_slice's brick groups, which count survivors rather than "
    "trusting the process to fail.\n"
    "\n"
    "A leak rather than a crash, which is the failure mode that hides: a flagged entity "
    "stops matching queries immediately, so nothing that reads the world sees it, and "
    "only a count of what is still *stored* notices.",
    [patch(PLAY_C, "    m_world.update();\n}", "    // MUTATION: the flush is gone\n}")],
)

mutation(
    "pause-lets-the-simulation-through",
    "The pause gate no longer returns, so every system runs in a paused frame.",
    "debug.controls' pause groups, and - the reason it belongs in *this* phase's harness "
    "as well - gameplay.a3_vertical_slice's *returning to the menu and starting again*, "
    "which pauses a level with a bullet in flight.",
    [patch(PLAY_C, "    if (m_paused)\n    {\n        return;\n    }\n\n    m_systems.update(",
           "    m_systems.update(")],
)

mutation(
    "menu-never-answers-a-transition",
    "`MenuScene` never asks for anything, so START does nothing.",
    "gameplay.a3_vertical_slice's *the menu starts the game*, and scene.lifecycle's "
    "transition groups.",
    [patch(MENU_C,
           "    if (actions.wasPressed(input::Action::Shoot))\n    {\n        if (m_selected == 0U)",
           "    if (false) // MUTATION: the menu answers nothing\n    {\n        if (m_selected == 0U)")],
)

# ---------------------------------------------------------------------------
# The level, which is data and therefore the only thing that can break "playable"
# ---------------------------------------------------------------------------

mutation(
    "no-brick-in-the-committed-level",
    "Both of the level's bricks are removed.",
    "gameplay.a3_vertical_slice's *the committed level carries what the slice needs* and "
    "*the player walks through where the brick was*, and combat.tiles' committed-level "
    "groups, and level_test's counts of the shipped level.\n"
    "\n"
    "A mutation on the **data**, not the code, which is the point: the phase's claim is "
    "about the level that ships.",
    [patch(LEVEL_TXT, "Tile mario_Brick_tile 8 1\nTile mario_Brick_tile 5 3\n", "")],
)

mutation(
    "the-low-brick-moves-out-of-reach",
    "The low brick moves from cell (8, 1) to cell (3, 1) - above the spawn, where a level "
    "designer would never put one.",
    "gameplay.a3_vertical_slice's brick groups, which address the tile by the position the "
    "level file gives it.\n"
    "\n"
    "A smaller change than deleting it and a more realistic one: somebody tidying a "
    "level. It also proves the groups read the level's coordinates rather than constants "
    "that happen to agree with today's file - except that they do not, and that is "
    "recorded rather than hidden.",
    [patch(LEVEL_TXT, "Tile mario_Brick_tile 8 1\n", "Tile mario_Brick_tile 3 1\n")],
)

mutation(
    "the-floor-has-no-hole",
    "The hole in the bottom row is filled in.",
    "gameplay.a3_vertical_slice's *the committed level carries what the slice needs*, "
    "which requires a cell the player can fall through.\n"
    "\n"
    "The fall and the respawn still work - `falling out of the world respawns the player* "
    "drops the player deliberately - so what this catches is narrower and worth catching: "
    "the *level* is what offers the fall, and filling the hole silently removes the only "
    "place a player could ever take it.",
    [patch(LEVEL_TXT, "Tile mario_ground_tile 18 0\n", "Tile mario_ground_tile 18 0\nTile mario_ground_tile 19 0\n")],
)

mutation(
    "the-player-spawns-inside-the-wall",
    "The player spawns at cell (8, 4), on the floating ledge.",
    "gameplay.a3_vertical_slice's *the player spawns from the level and falls onto the "
    "floor*, which restates the level's own spawn cell rather than reading it.\n"
    "\n"
    "The failure a playable-slice phase is supposed to catch, and the one no system test "
    "can: the level puts the player somewhere they cannot be.",
    [patch(LEVEL_TXT, "Player 3 4 40 60 200 400 250 900 megaman_megaBuster_shot",
           "Player 8 4 40 60 200 400 250 900 megaman_megaBuster_shot")],
)

mutation(
    "the-players-jump-speed-is-reduced",
    "The level's jump speed drops from 400 to 250, which is its own maximum.",
    "gameplay.a3_vertical_slice's *the player jumps and lands on the same floor*, which "
    "compares the rise with `jumpSpeed^2 / (2 * gravity)`, and player.behaviour's jump "
    "groups.\n"
    "\n"
    "The course's own note in [engine::systems::PlayerSystem] is that clamping the "
    "vertical axis too would make a jump of 34 pixels - smaller than the player's own "
    "60 - so this is a tuning value with a documented consequence rather than a free "
    "parameter.",
    [patch(LEVEL_TXT, "Player 3 4 40 60 200 400 250 900", "Player 3 4 40 60 200 250 250 900")],
)

# ---------------------------------------------------------------------------
# The scene lifecycle the slice is played inside
# ---------------------------------------------------------------------------

mutation(
    "a-respawn-does-not-restore-the-facing",
    "`PlayerSystem`'s respawn leaves the player's facing as it was, so a respawn after "
    "running left leaves the player facing left.",
    "gameplay.a3_vertical_slice: *falling out of the world respawns the player*, which "
    "resets `scale.x` and reads it back, and player.behaviour's respawn groups.\n"
    "\n"
    "The first version of this mutation tried to make the *scene* inherit the flag from a "
    "previous scene, which is not valid C++ - `m_paused` is declared after the thing that "
    "would read it. Phase 19 already covers the shared-flag mistake properly; this is "
    "the integration-level leak instead, and it is one a real run hits every time a "
    "player falls.",
    [patch(f"{SRC}/systems/PlayerSystem.cpp",
           "    transform.scale = Vec2{1.0F, 1.0F};",
           "    // MUTATION: the facing survives a respawn\n"
           "    transform.scale.x = std::abs(transform.scale.x);")],
)

mutation(
    "the-camera-system-is-not-registered",
    "`CameraSystem` is not registered, so the view never follows the player.",
    "gameplay.a3_vertical_slice's camera groups and its *the whole chain in one pass*, and "
    "scene.lifecycle's system count and order.\n"
    "\n"
    "A camera that does not move still renders a world - the same entities, at their own "
    "positions - so every drawing assertion in the repository passes. Only a comparison "
    "between the camera and the player can see it.",
    [patch(PLAY_C,
           "    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});\n"
           "    // Local class, not an engine one - see its own documentation above.\n"
           "    m_systems.add<ZoomKeysSystem>(context.camera());\n",
           "    // MUTATION: the view never follows anybody\n"
           "    // Local class, not an engine one - see its own documentation above.\n"
           "    m_systems.add<ZoomKeysSystem>(context.camera());\n")],
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
    result = subprocess.run(["ctest", "--output-on-failure", "--timeout", "900"],
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
    """The shipped binary, twice: plain, and playing itself.

    The second one is the check a mutation harness would otherwise have no way to make,
    because a scripted run and a silent one both exit zero.
    """
    binary = os.path.join(BUILD, "game")
    if not os.path.exists(binary):
        return None, "no game binary"
    try:
        plain = subprocess.run([binary, "--frames", "5"], cwd=ROOT, capture_output=True,
                               text=True, timeout=300)
        scripted = subprocess.run(
            [binary, "--frames", "300", "--keys", "space .:60 D:60 space .:60"],
            cwd=ROOT, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    return (plain.returncode, scripted.returncode), (plain.stdout + plain.stderr)


def failing_checks(output):
    found = {}
    for match in re.finditer(r"^\s*(\S+\.cpp):(\d+): CHECK\((.*?)\) failed", output, re.MULTILINE):
        found.setdefault(os.path.basename(match.group(1)), []).append(
            "{}:{}".format(match.group(2), match.group(3)))
    return found


def classify(clean_failed, abnormal, build_ok, timeouts, equivalent):
    """The five outcomes, decided by what the run actually did.

    A clean `Failed` outranks a crash *somewhere else*: declaring a static member makes
    every suite that builds a scene abort, and several also fail with real assertion
    failures first. Reporting CRASHED alone would throw those detections away.
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
                game_codes, _ = run_game()
                if game_codes is not None and game_codes != (0, 0):
                    abnormal["game"] = "exit {}".format(game_codes)
                outcome = classify(clean_failed, abnormal, built, timeouts, entry.get("equivalent"))
                detail = json.dumps({"clean_failed": clean_failed, "abnormal": abnormal,
                                     "game_exits": game_codes,
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