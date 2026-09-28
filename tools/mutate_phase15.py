#!/usr/bin/env python3
"""Mutation testing for Phase 15.

Design notes, because a mutation harness that lies is worse than none:

* Every mutation is applied to a file, and the file is *read back and compared*
  before the build starts. A patch that silently matched nothing would produce a
  green run and be recorded as EQUIVALENT, which is the most expensive way to be
  wrong.
* Every mutation is built in its own build directory, configured from scratch, so a
  mutation that only needs one file recompiled is still a real compile and a stale
  object can never make a failing mutation look green.
* Every mutation runs *all* nineteen ctest entries plus the game binary. The list is
  asserted to be the full list, because a harness that quietly skipped a suite would
  report UNDETECTED for a mutation that is in fact caught.
* Outcomes are classified by what actually happened, not by what was hoped for. A
  test binary that exits non-zero having printed "check(s) failed" is TEST_DETECTED.
  A binary that dies on a signal, or a ctest that times out, is CRASHED - reported
  separately, never counted as a detection, because a segfault is not an assertion.
  A compile error is BUILD_REJECTED. All-green is EQUIVALENT, and is investigated.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-mut")
RESULTS_PATH = os.environ.get(
    "MUTATION_RESULTS",
    os.path.join(tempfile.gettempdir(), "phase15-mutation-results.json"))

# The full set of suites. Read from `ctest -N` rather than written from memory: a
# harness that expects the wrong number of tests would pass a mutation that only
# fails in a suite it never ran. `main` verifies this list against the real one.
EXPECTED_TESTS = [
    "application.smoke",
    "application.end_to_end",
    "math.vec2",
    "ecs.core",
    "ecs.systems",
    "runtime.timing_transform",
    "render.foundation",
    "animation.playback",
    "input.action_abstraction",
    "render.text",
    "scene.lifecycle",
    "level.format_and_spawn",
    "input.keyboard_movement",
    "physics.collision",
    "camera.world_to_screen",
    "assets.configuration_parser",
    "assets.handles",
    "assets.manager_interface",
    "assets.loading",
]

MUTATIONS = []


def mutation(name, description, intent, patches):
    MUTATIONS.append({
        "name": name,
        "description": description,
        "intent": intent,
        "patches": patches,
    })


def patch(path, old, new, count=1):
    return {"path": path, "old": old, "new": new, "count": count}


INC = "include/engine"
SRC = "src/engine"

# ---------------------------------------------------------------------------
# Transitions
# ---------------------------------------------------------------------------

mutation(
    "transition-disabled",
    "The owner never acts on a transition request.",
    "scene_test: 'the transition happens at a frame boundary', "
    "'a transition destroys the old scene', 'no scene is destroyed while it is running'",
    [patch(f"{SRC}/Application.cpp",
           "    if (!m_pendingTransition.has_value())\n    {\n        return; // No scene asked",
           "    if (true)\n    {\n        return; // MUTATION: transition disabled")],
)

mutation(
    "transition-immediate",
    "The request is applied inside update(), right after the scene runs, rather than at the frame boundary.",
    "scene_test: 'the transition happens at a frame boundary' (the frame that asked "
    "would also be the frame that drew the new scene)",
    [patch(f"{SRC}/Application.cpp",
           "    applyPendingTransition();\n",
           "")],
)

# Insert the immediate application *after* the scene's own update, which is the
# variant that genuinely destroys a scene between its update and its render.
mutation(
    "transition-between-update-and-render",
    "The request is applied after the scene's update but before its render.",
    "scene_test: 'the transition happens at a frame boundary' and "
    "'no scene is destroyed while it is running'",
    [patch(f"{SRC}/Application.cpp",
           "        m_scene->update(m_actions, m_time.deltaSeconds());\n",
           "        m_scene->update(m_actions, m_time.deltaSeconds());\n"
           "        applyPendingTransition(); // MUTATION: applied mid-frame\n")],
)

mutation(
    "transition-to-wrong-scene",
    "A request for the play scene builds the menu instead.",
    "scene_test: 'sceneId is asked of the scene', 'a transition destroys the old scene', "
    "'the menu transitions to play and back'",
    [patch(f"{SRC}/scene/Scene.cpp",
           "        case SceneId::Play:\n            return std::make_unique<PlayScene>(context);",
           "        case SceneId::Play:\n"
           "            return std::make_unique<MenuScene>(context); // MUTATION: wrong scene")],
)

mutation(
    "transition-records-request-not-scene",
    "The owner remembers the requested id instead of asking the scene which one it is.",
    "scene_test: 'sceneId is asked of the scene' - a factory that ignores its argument "
    "must not be able to make sceneId() lie",
    [patch(f"{INC}/Application.hpp",
           "    std::optional<scene::SceneTransition> m_pendingTransition;",
           "    std::optional<scene::SceneTransition> m_pendingTransition;\n"
           "    std::optional<scene::SceneId> m_rememberedId; // MUTATION"),
     patch(f"{SRC}/Application.cpp",
           "    m_pendingTransition = scene::SceneTransition::to(id);",
           "    m_pendingTransition = scene::SceneTransition::to(id);\n"
           "    m_rememberedId = id; // MUTATION: remembered rather than asked"),
     patch(f"{SRC}/Application.cpp",
           "    return m_scene->id();",
           "    return m_rememberedId; // MUTATION")],
)

mutation(
    "quit-ignored",
    "A quit request is applied but the running flag is not cleared.",
    "scene_test: 'a quit request stops the application'",
    [patch(f"{SRC}/Application.cpp",
           "        m_isRunning = false;\n        return;",
           "        return; // MUTATION: quit ignored")],
)

mutation(
    "quit-degraded-into-a-scene-switch",
    "quitApplication() returns a scene switch instead of a quit, so 'QUIT' on the menu "
    "navigates into the level.",
    "scene_test: 'the menu quits' - a quit is a third answer, not a scene",
    [patch(f"{INC}/scene/Scene.hpp",
           "        return SceneTransition{std::nullopt, true};",
           "        return SceneTransition{std::optional<SceneId>{SceneId::Menu}, false}; // MUTATION")],
)

mutation(
    "scene-holds-a-back-pointer-to-the-owner",
    "A scene gains a void* owner pointer, so the type that is supposed to be unable to "
    "reach the engine can.",
    "BUILD_REJECTED is not expected and TEST_DETECTED is not expected either - this is "
    "the shape of change a *review* catches and a test cannot, which is why the "
    "no-global group is a source check rather than a behavioural one. Recorded as "
    "UNDETECTED deliberately, so the limitation is visible rather than assumed away.",
    [patch(f"{INC}/scene/Scene.hpp",
           "    const SceneContext* m_context;",
           "    const SceneContext* m_context;\n    void* m_owner = nullptr; // MUTATION")],
)

# ---------------------------------------------------------------------------
# One active scene
# ---------------------------------------------------------------------------

mutation(
    "inactive-scene-updated",
    "The scene's update is called and then the application's own systems run as well.",
    "scene_test: 'only the active scene updates' and "
    "'scenes do not register into the application'",
    [patch(f"{SRC}/Application.cpp",
           "        m_scene->update(m_actions, m_time.deltaSeconds());\n    }",
           "        m_scene->update(m_actions, m_time.deltaSeconds());\n"
           "        m_systemManager.update(m_entityManager, m_actions, m_time.deltaSeconds());\n"
           "        m_entityManager.update();\n"
           "    } // MUTATION: both worlds run")],
)

mutation(
    "inactive-scene-rendered",
    "The scene renders and then the render system draws the application's own world too.",
    "scene_test: 'only the active scene renders'",
    [patch(f"{SRC}/Application.cpp",
           "        m_scene->render();\n    }",
           "        m_scene->render();\n"
           "        m_renderSystem.update(m_entityManager, m_actions, 0.0F);\n"
           "    } // MUTATION: both worlds draw")],
)

mutation(
    "old-scene-kept-alive",
    "The old scene is moved into a local list instead of destroyed, so it is not "
    "released on transition.",
    "scene_test: 'a transition destroys the old scene' and "
    "'the world of an old scene is gone'",
    [patch(f"{SRC}/Application.cpp",
           "    m_scene.reset();",
           "    // MUTATION: the old scene is kept rather than released\n"
           "    static std::vector<std::unique_ptr<scene::Scene>> graveyard;\n"
           "    graveyard.push_back(std::move(m_scene));\n    m_scene.reset();")],
)

mutation(
    "scene-request-never-cleared",
    "A scene's pending transition is not cleared each frame, so a request repeats forever.",
    "BUILD_REJECTED expected: the whole point of the template method is that the "
    "clear is in the base class and cannot be forgotten",
    [patch(f"{INC}/scene/Scene.hpp",
           "        m_transition = SceneTransition{};\n\n        onUpdate(actions, deltaSeconds);",
           "        onUpdate(actions, deltaSeconds); // MUTATION: request never cleared")],
)

# ---------------------------------------------------------------------------
# Menu behaviour
# ---------------------------------------------------------------------------

mutation(
    "menu-confirms-on-jump",
    "The menu confirms on Jump instead of Shoot.",
    "scene_test: 'the menu starts the game' - pressing Space drives Shoot alone, so "
    "a Jump-confirming menu asks for nothing",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "    if (actions.wasPressed(input::Action::Shoot))",
           "    if (actions.wasPressed(input::Action::Jump)) // MUTATION")],
)

mutation(
    "menu-navigates-with-held-key",
    "The menu reads isActive instead of wasPressed, so a held key runs the selection "
    "round every frame.",
    "scene_test: 'the menu moves on the press edge only'",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "    if (actions.wasPressed(input::Action::MoveUp))",
           "    if (actions.isActive(input::Action::MoveUp)) // MUTATION")],
)

mutation(
    "menu-quit-option-starts-the-game",
    "The second menu option starts the play scene instead of quitting.",
    "scene_test: 'the menu quits'",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "            requestTransition(SceneTransition::quitApplication());",
           "            requestTransition(SceneTransition::to(SceneId::Play)); // MUTATION")],
)

mutation(
    "menu-uses-raw-keys",
    "The menu reaches for the keyboard instead of the action snapshot.",
    "scene_test: 'scenes name no keys and read actions' - a source check, and the "
    "only kind of change it can catch",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "#include \"engine/input/Action.hpp\"",
           "#include \"engine/input/Action.hpp\"\n"
           "#include <SFML/Window/Keyboard.hpp> // MUTATION")],
)

mutation(
    "menu-not-marked-screen-space",
    "The menu's labels are not marked ScreenSpace, so the camera applies to them.",
    "scene_test: 'the menu text is screen space', 'camera movement does not move "
    "menu text', 'camera zoom does not change menu text'",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "    entity.addComponent<components::ScreenSpace>();",
           "    // MUTATION: not screen space")],
)

mutation(
    "menu-has-no-text",
    "The menu's title is built with an empty string, so it draws nothing.",
    "scene_test: 'the menu can be created' and 'the menu renders real text'",
    [patch(f"{SRC}/scene/MenuScene.cpp",
           "    addLabel(\"menu.title\", \"GAME ENGINE\", kTitleSize, Vec2{centreX, viewport.y * kTitleHeight});",
           "    addLabel(\"menu.title\", \"\", kTitleSize, Vec2{centreX, viewport.y * kTitleHeight}); // MUTATION")],
)

# ---------------------------------------------------------------------------
# Screen space - the two guarantees
# ---------------------------------------------------------------------------

mutation(
    "screen-space-through-camera",
    "An entity marked ScreenSpace is drawn through the camera like any other.",
    "scene_test: 'camera movement does not move menu text', 'camera zoom does not "
    "change menu text', 'world text still follows the camera'",
    [patch(f"{SRC}/systems/RenderSystem.cpp",
           "        const graphics::RenderTransform placement = entity.hasComponent<components::ScreenSpace>()\n"
           "                                                         ? graphics::toScreenTransform(transform)\n"
           "                                                         : graphics::toRenderTransform(transform, *m_camera);",
           "        const graphics::RenderTransform placement =\n"
           "            graphics::toRenderTransform(transform, *m_camera); // MUTATION")],
)

mutation(
    "screen-space-inverted",
    "The marker inverts the rule: unmarked text is screen space and marked text is world.",
    "scene_test: every screen-space group, plus 'the menu text is screen space'",
    [patch(f"{SRC}/systems/RenderSystem.cpp",
           "        const graphics::RenderTransform placement = entity.hasComponent<components::ScreenSpace>()\n"
           "                                                         ? graphics::toScreenTransform(transform)\n"
           "                                                         : graphics::toRenderTransform(transform, *m_camera);",
           "        const graphics::RenderTransform placement = !entity.hasComponent<components::ScreenSpace>()\n"
           "                                                         ? graphics::toScreenTransform(transform)\n"
           "                                                         : graphics::toRenderTransform(transform, *m_camera); // MUTATION")],
)

mutation(
    "screen-space-applies-a-constant-scale",
    "toScreenTransform multiplies the scale by a constant, so screen-space placement "
    "stops being the entity's own scale.",
    "scene_test: 'screen space still applies scale and rotation'. The original version "
    "of this mutation gave toScreenTransform a Camera parameter, which is BUILD_"
    "REJECTED - and rightly so, because two call sites in the suite pin the "
    "one-argument signature and a signature change is the compiler's job to refuse. "
    "The behaviour worth mutating is reachable without touching the signature, and "
    "the camera-zoom path itself is covered by screen-space-through-camera and "
    "screen-space-inverted, which both detect.",
    [patch(f"{INC}/graphics/RenderTransform.hpp",
           "constexpr RenderTransform toScreenTransform(const components::Transform& transform) noexcept\n"
           "{\n"
           "    return RenderTransform{transform.position, transform.scale, transform.angle * kDegreesPerRadian};\n}",
           "constexpr RenderTransform toScreenTransform(const components::Transform& transform) noexcept\n"
           "{\n"
           "    return RenderTransform{transform.position, transform.scale * 2.0F,\n"
           "                           transform.angle * kDegreesPerRadian}; // MUTATION\n}")],
)

mutation(
    "screen-space-constant-rotation",
    "toScreenTransform reports a fixed rotation of zero.",
    "scene_test: 'screen space still applies scale and rotation' - the degrees "
    "conversion is the only thing asserted about the angle there",
    [patch(f"{INC}/graphics/RenderTransform.hpp",
           "constexpr RenderTransform toScreenTransform(const components::Transform& transform) noexcept\n"
           "{\n"
           "    return RenderTransform{transform.position, transform.scale, transform.angle * kDegreesPerRadian};\n}",
           "constexpr RenderTransform toScreenTransform(const components::Transform& transform) noexcept\n"
           "{\n"
           "    return RenderTransform{transform.position, transform.scale, 0.0F}; // MUTATION\n}")],
)

# ---------------------------------------------------------------------------
# The play scene
# ---------------------------------------------------------------------------

mutation(
    "play-does-not-go-back",
    "The play scene ignores Quit, so Escape does nothing.",
    "scene_test: 'the play scene goes back to the menu'",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    if (actions.wasPressed(input::Action::Quit))",
           "    if (false) // MUTATION: never goes back")],
)

mutation(
    "play-movement-system-registered",
    "The play scene registers a movement system, which would give every tile and "
    "decoration a velocity.",
    "scene_test: 'the play scene registers its own systems'",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "#include \"engine/systems/PhysicsSystem.hpp\"",
           "#include \"engine/systems/MovementSystem.hpp\"\n"
           "#include \"engine/systems/PhysicsSystem.hpp\""),
     patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_systems.add<engine::systems::PhysicsSystem>();",
           "    m_systems.add<engine::systems::PhysicsSystem>();\n"
           "    m_systems.add<engine::systems::MovementSystem>(100.0F); // MUTATION")],
)

mutation(
    "play-registers-no-systems",
    "The play scene registers none of its own systems.",
    "scene_test: 'the play scene registers its own systems' and "
    "'the play scene updates and renders'",
    # The registration block has an explanatory comment between the CameraSystem and
    # ZoomKeysSystem lines, so the four `add` calls are not contiguous. Two separate
    # patches neutralise the first and the last, which is the same observable change
    # as deleting all four: the count drops from four to two.
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_systems.add<engine::systems::PhysicsSystem>();",
           "    // MUTATION: not registered"),
     patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_systems.add<engine::systems::AnimationSystem>(context.assets());",
           "    // MUTATION: not registered")],
)

mutation(
    "play-skips-deferred-cleanup",
    "The play scene does not flush its own world's deferred destructions.",
    "scene_test: 'the play scene flushes its own deferred destruction'",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    m_world.update();",
           "    // MUTATION: the scene never flushes its own world")],
)

mutation(
    "play-adds-a-second-label",
    "The play scene adds a second text label, so the shipped demonstration is no "
    "longer one thing.",
    "scene_test: 'the play scene builds a world'",
    [patch(f"{SRC}/scene/PlayScene.cpp",
           "    addTextLabel(m_world,",
           "    addTextLabel(m_world, \"EXTRA\", Vec2{0.0F, 0.0F}); // MUTATION\n"
           "    addTextLabel(m_world,")],
)

# ---------------------------------------------------------------------------
# Ownership
# ---------------------------------------------------------------------------

mutation(
    "scene-owns-its-own-asset-manager",
    "The play scene builds its own asset manager and uses that, instead of the shared "
    "one it is given.",
    "scene_test: 'assets are shared not duplicated' - the assertion is pointer "
    "identity against the application's own manager, not merely that both can load",
    # The first attempt made the *loader* use a second manager, and it went
    # UNDETECTED: `context().assets()` was still the shared one, so the pointer-identity
    # assertion held and only the loader's private choice had changed. The duplication
    # that matters is the one the scene can *see*, so the owner hands the context a
    # second asset manager - and then a menu loads every texture and every font a
    # second time, which is the thing the shared table exists to prevent.
    [patch(f"{INC}/Application.hpp",
           "    assets::SfmlAssetManager m_assets;",
           "    assets::SfmlAssetManager m_assets;\n"
           "    assets::SfmlAssetManager m_duplicateAssets; // MUTATION"),
     patch(f"{SRC}/Application.cpp",
           "      m_assets{std::filesystem::path{config::kAssetsConfig}},",
           "      m_assets{std::filesystem::path{config::kAssetsConfig}},\n"
           "      m_duplicateAssets{std::filesystem::path{config::kAssetsConfig}}, // MUTATION"),
     patch(f"{SRC}/Application.cpp",
           "      m_sceneContext{m_renderer, m_camera, m_assets},",
           "      m_sceneContext{m_renderer, m_camera, m_duplicateAssets}, // MUTATION")],
)

mutation(
    "global-scene-manager",
    "A file-scope static scene that every caller shares.",
    "scene_test: 'there is no global scene manager'",
    [patch(f"{SRC}/scene/Scene.cpp",
           "std::unique_ptr<Scene> makeScene(const SceneId id, const SceneContext& context)",
           "static std::unique_ptr<Scene> g_currentScene; // MUTATION: a global\n"
           "std::unique_ptr<Scene> makeScene(const SceneId id, const SceneContext& context)")],
)

mutation(
    "scene-header-names-sfml",
    "The scene base header names an SFML type.",
    "scene_test: 'the scene headers are sfml free' - a source check",
    [patch(f"{INC}/scene/Scene.hpp",
           "#include \"engine/input/ActionState.hpp\"",
           "#include \"engine/input/ActionState.hpp\"\n"
           "#include <SFML/Graphics/RenderWindow.hpp> // MUTATION")],
)

mutation(
    "renderer-knows-about-scenes",
    "The renderer interface names a scene id, so a renderer could switch worlds "
    "mid-draw.",
    "scene_test: 'the engine layers below do not know about scenes'",
    [patch(f"{INC}/graphics/Renderer.hpp",
           "namespace engine::graphics\n{\n",
           "namespace engine::scene { enum class SceneId; } // MUTATION\n"
           "namespace engine::graphics\n{\n")],
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
    """Applies every patch, verifying each one matched. Raises on any miss."""
    applied = []
    for spec in entry["patches"]:
        original = read(spec["path"])
        occurrences = original.count(spec["old"])
        if occurrences != spec["count"]:
            raise RuntimeError(
                "patch target found {} time(s) in {}, expected {}: {!r}".format(
                    occurrences, spec["path"], spec["count"], spec["old"][:70]))
        write(spec["path"], original.replace(spec["old"], spec["new"]))
        applied.append(spec["path"])
    return applied


def revert(entry):
    """Restores every touched file with git, not with string replacement.

    The first version replaced `new` with `old` in the file it had written. That is
    wrong the moment a mutation *deletes* text, because its `new` is the empty
    string and `str.replace("", old)` splices `old` between every character of the
    file. It destroyed a 700-line source file and reported a green run, which is
    exactly the failure a mutation harness must not have.

    `git checkout` cannot make that mistake: the file is restored byte for byte from
    HEAD, and `git status` afterwards says whether anything is still modified.
    """
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


def run_all_tests():
    """Runs every ctest entry. Returns (failing test names, ran names, any_timeout)."""
    result = subprocess.run(["ctest", "--output-on-failure", "--timeout", "300"],
                            cwd=BUILD, capture_output=True, text=True)
    output = result.stdout + result.stderr

    ran = re.findall(r"^\s*\d+/\d+ Test\s+#\d+:\s+(\S+)", output, re.MULTILINE)

    # The failing list is read from ctest's own summary block, which is the one place
    # the names are unambiguous. The per-test progress line was the first thing tried
    # and it never matched: ctest writes "name .....***Failed", so the dots sit where
    # the pattern expected whitespace, and every failing mutation was reported as
    # green. That is the single most expensive bug a mutation harness can have, and
    # the reason the two forms are both spelled out here.
    # ctest reports the reason in brackets, and the reason is not always "Failed".
    # The first version of this matched `\(Failed\)` only, so a suite that
    # **segfaulted** was reported as no failure at all - and `transition-disabled`
    # was filed as UNDETECTED when in fact the test binary was dying. Matching only
    # the clean case is how a crash gets mistaken for a pass, so every reason ctest
    # can print is captured and classified separately below.
    crashed = {}
    for name, reason in re.findall(r"^\s*\d+ - (\S+) \((\S+(?: \S+)*)\)", output, re.MULTILINE):
        if reason != "Passed":
            crashed[name] = reason

    timeouts = bool(re.search(r"\*\*\*Timeout|\bTimeout\b", output))

    # A clean "Failed" is the only thing that counts as a detection. Everything else
    # ctest can report - a signal, a timeout, an unhandled exception - is a crash,
    # and a crash is not a detection: it says something broke, not that an assertion
    # noticed.
    clean_failed = [name for name, reason in crashed.items() if reason == "Failed"]
    abnormal = {name: reason for name, reason in crashed.items() if reason != "Failed"}

    return clean_failed, abnormal, ran, timeouts, output


def run_game():
    """Runs the game binary. Returns (exit code, output)."""
    binary = os.path.join(BUILD, "game")
    if not os.path.exists(binary):
        return None, "no game binary"
    result = subprocess.run([binary, "--frames", "5"], cwd=ROOT, capture_output=True,
                            text=True, timeout=300)
    return result.returncode, (result.stdout + result.stderr)


def failing_checks(output):
    """The CHECK expressions that failed, per suite, so a human can confirm intent."""
    found = {}
    for match in re.finditer(r"^\s*(\S+\.cpp):(\d+): CHECK\((.*?)\) failed", output,
                             re.MULTILINE):
        found.setdefault(match.group(1), []).append(f"{match.group(2)}:{match.group(3)}")
    return found


def classify(clean_failed, abnormal, ran_suites, build_ok, timeouts):
    """The five outcomes, decided by what the run actually did.

    A crash is deliberately *not* a detection. A segfaulting test binary means the
    suite did not survive to assert anything, and reporting that as "the mutation
    was caught" would be exactly the dishonest claim this harness exists to avoid.
    It gets its own bucket so it is impossible to overlook.
    """
    if not build_ok:
        return "BUILD_REJECTED"
    if len(ran_suites) != len(EXPECTED_TESTS):
        raise RuntimeError("harness ran {} of {} suites: {}".format(
            len(ran_suites), len(EXPECTED_TESTS), ran_suites))
    if timeouts or abnormal:
        return "CRASHED"
    if clean_failed:
        return "TEST_DETECTED"
    return "UNDETECTED"


def verify_green():
    """The reverted tree must be green before the next mutation is measured.

    Without this, a mutation can inherit the previous one's broken binary and be
    reported against a build it never had.
    """
    clean, abnormal, ran, timeouts, _ = run_all_tests()
    if clean or abnormal or timeouts:
        raise RuntimeError("reverted tree is not green: clean={} abnormal={} timeouts={}".format(
            clean, abnormal, timeouts))
    if len(ran) != len(EXPECTED_TESTS):
        raise RuntimeError("reverted tree ran {} of {} suites".format(len(ran), len(EXPECTED_TESTS)))


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

    build_ok, build_output = build()
    if not build_ok:
        print(build_output[-4000:])
        raise SystemExit("baseline build failed")

    base_clean, base_abnormal, baseline_ran, base_timeouts, _ = run_all_tests()
    if base_clean or base_abnormal or base_timeouts:
        raise SystemExit("baseline is not green: {} {}".format(base_clean, base_abnormal))
    if sorted(baseline_ran) != sorted(EXPECTED_TESTS):
        raise SystemExit("harness expectation is wrong.\n  ctest reports: {}\n  harness expects: {}".format(
            sorted(baseline_ran), sorted(EXPECTED_TESTS)))
    print("baseline: {} of {} suites green; the game binary is run per mutation too".format(
        len(baseline_ran), len(EXPECTED_TESTS)), flush=True)

    results = []
    for entry in entries:
        started = time.time()
        print("=== {} ===".format(entry["name"]), flush=True)
        reverted = False
        try:
            touched = apply_patches(entry)
            # Read back: a patch that matched nothing would otherwise be a silent
            # EQUIVALENT, which is the most expensive way for this harness to be
            # wrong.
            for spec in entry["patches"]:
                if spec["new"] not in read(spec["path"]):
                    raise RuntimeError("patch did not survive write-back: {}".format(spec["path"]))

            ok, build_out = build()
            if not ok:
                outcome = "BUILD_REJECTED"
                detail = build_out.strip().splitlines()
                detail = detail[-1] if detail else ""
                clean_failed, abnormal, ran, timeouts = [], {}, [], False
            else:
                clean_failed, abnormal, ran, timeouts, test_out = run_all_tests()
                game_code, game_out = run_game()
                if game_code is not None and game_code != 0:
                    abnormal["game"] = "exit {}".format(game_code)
                outcome = classify(clean_failed, abnormal, ran, ok, timeouts)
                detail = json.dumps({
                    "clean_failed": clean_failed,
                    "abnormal": abnormal,
                    "game_exit": game_code,
                })
                detail += "\n    " + json.dumps(
                    {os.path.basename(k): v for k, v in failing_checks(test_out).items()})[:1400]
        except Exception as error:  # noqa: BLE001
            outcome = "HARNESS_ERROR"
            detail = str(error)
            clean_failed, abnormal, ran, timeouts = [], {}, [], False
        finally:
            revert(entry)
            reverted = True
        if not reverted:
            revert(entry)
        # Rebuild so the next mutation starts from a clean, correct tree - and check
        # that it actually succeeded.
        #
        # This is not defensive noise. An earlier run reported
        # `scene-holds-a-back-pointer-to-the-owner` as CRASHED when it detects cleanly
        # in isolation, because a previous mutation's revert-and-rebuild had not
        # completed and the next mutation's ctest ran against a stale binary. A
        # mutation result is only worth as much as the build it was measured on, so a
        # failed rebuild is now a hard error rather than a wrong verdict.
        restored_ok, restored_output = build()
        if not restored_ok:
            raise RuntimeError("rebuild after reverting {} failed:\n{}".format(
                entry["name"], restored_output[-2000:]))
        verify_green()

        results.append({
            "name": entry["name"],
            "description": entry["description"],
            "intent": entry["intent"],
            "outcome": outcome,
            "clean_failed": clean_failed,
            "abnormal": abnormal,
            "detail": detail,
            "seconds": round(time.time() - started, 1),
        })
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
            print("    - {}".format(name))
    print("total: {} mutations".format(len(results)))


if __name__ == "__main__":
    main()
