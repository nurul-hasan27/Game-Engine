#pragma once

#include "engine/debug/DebugRenderState.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/systems/DebugRenderSystem.hpp"
#include "engine/systems/RenderSystem.hpp"

namespace engine::scene
{

/// A level being played.
///
/// ### What it owns
///
/// Its own [ecs::EntityManager] and its own [ecs::SystemManager], both of which
/// disappear with the scene. That is the point of a scene boundary: the menu's four
/// entities cannot still be in the world when a level loads, and a level's
/// hundreds of entities cannot leak into the next one.
///
/// What it does **not** own is the renderer, the asset manager or the camera. Those
/// belong to the owner and are borrowed through the [SceneContext], so switching
/// scenes does not reload a single font.
///
/// ### Why it registers its own systems
///
/// Because a system's job is to act on a world, and the world belongs to the scene.
/// Registering them here rather than in [engine::Application] preserves the
/// decision this engine made in Phase 6 - that the engine assembles no systems of
/// its own - and makes the ownership obvious: a system is destroyed with the world
/// it was registered for.
///
/// ### No player behaviour
///
/// The player does not move yet, and the absence is deliberate. The engine's
/// `MovementSystem` sets a velocity on *every* entity with a transform, so
/// registering it here would set a velocity on every ground tile and every
/// decoration; a decoration has no body, so physics would integrate it and the
/// scenery would slide around the screen. Making the player move needs a movement
/// system that knows which entity is the player, and that is the next phase's job.
///
/// ### The debug controls, and where they live
///
/// The course's Assignment 3 asks for five keys in this scene, and this class is
/// where all five are honoured:
///
/// | Key | Action | What it does here |
/// | --- | ------ | ----------------- |
/// | `P` | [input::Action::Pause] | freezes and unfreezes the simulation |
/// | `T` | [input::Action::ToggleTextures] | suppresses entity images |
/// | `C` | [input::Action::ToggleBoundingBoxes] | draws every collider box |
/// | `G` | [input::Action::ToggleGrid] | draws the 64-pixel level grid |
/// | `ESC` | [input::Action::Quit] | asks for the menu scene |
///
/// Four of the five are *this* class's business rather than a system's, and the
/// reason is the same for all of them: **a debug control is a decision, not a rule.**
/// A system cannot decide whether the game is paused, because it has no way to stop
/// the systems after it; it can only decide something inside its own frame. So the
/// flags live here, in the one object that owns the frame, and the only two things
/// that ever read them are the two renderers.
///
/// ### Pause is one branch here and not a flag in nine systems
///
/// [paused](PlayScene.hpp) is a single `bool` read in exactly one place - the gate
/// between the debug controls and [m_systems](PlayScene.hpp) - because that is the
/// only place from which "do not simulate this frame" can be said without every
/// gameplay system growing a debug-specific early return. No system below knows
/// pause exists: [engine::systems::PhysicsSystem] integrates, [engine::systems::LifetimeSystem]
/// counts and [engine::systems::AnimationSystem] advances unconditionally, exactly as
/// they do when the game is running, and the scene simply does not call them.
///
/// ### The render state is the render system's, and that is where the answer is
///
/// The three rendering flags are reached through
/// [debugRenderState](PlayScene.hpp), which is a forwarding accessor onto
/// [engine::systems::RenderSystem]'s own state. It is not a member here because that
/// would leave two objects with three flags between them and no single answer to
/// "are the overlays on this frame" - and the renderer and the debug renderer must
/// agree about it. See
/// [engine::debug::DebugRenderState](engine/debug/DebugRenderState.hpp).
///
/// ### Scene lifetime is the whole of the reset story
///
/// There is no `reset()` and nothing to un-toggle. The flags are members of this
/// object, and [engine::Application] destroys the scene on every transition, so
/// coming back to the menu and starting again builds a new scene with textures on,
/// no overlays and no pause - which is what "a fresh gameplay session" means when
/// the world is rebuilt from the level file on every entry. See
/// [engine::scene::SceneTransition] for why the old scene really is gone rather
/// than merely forgotten.
class PlayScene final : public Scene
{
public:
    /// Loads the configured level and registers the systems that run in it.
    ///
    /// @throws engine::level::LevelParseError if the level file is malformed.
    /// @throws engine::assets::AssetNotFoundError if the level names an animation
    ///         that is not declared. Both are deliberate: a level that loads half
    ///         way is worse than one that refuses to load.
    explicit PlayScene(const SceneContext& context);

    /// Runs the scene's systems, in registration order.
    ///
    /// Overrides [Scene::onUpdate] rather than `update`, because the base class
    /// clears the pending request before calling this. See [Scene::update] for why
    /// that is not left to each scene.
    void onUpdate(const input::ActionState& actions, float deltaSeconds) override;

    /// Submits the level's world to the shared renderer.
    void render() override;

    [[nodiscard]] const char* name() const noexcept override { return "PlayScene"; }

    [[nodiscard]] SceneId id() const noexcept override { return SceneId::Play; }

    /// The level's own world, for tests and for spawning into the scene.
    ///
    /// ### Both a const and a mutable accessor, and why the mutable one exists
    ///
    /// The const overload is for inspection. The mutable one is for putting things
    /// *into* the level, which is not a test convenience: a game that spawns a
    /// bullet or a pickup needs somewhere to add it, and the scene owns the world
    /// that has to happen in. Reaching for [engine::Application::entityManager]
    /// instead would be reaching the wrong world - the application's, which while a
    /// scene is active is not the frame's subject and is never drawn.
    ///
    /// The scene still *owns* the world: handing out a reference to a member is not
    /// giving up ownership, and it is the same thing `Application::entityManager`
    /// already does for its own.
    ///
    /// It is also what makes the deferred-destruction guarantee observable. The
    /// committed level contains nothing that is ever destroyed - every animation it
    /// uses is single frame and repeating, because a ground tile that vanished when
    /// its one frame elapsed would be a bug - so a scene's flush is a no-op until
    /// something non-repeating is in the world. A test cannot demonstrate that from
    /// a const view.
    [[nodiscard]] ecs::EntityManager& world() noexcept { return m_world; }
    [[nodiscard]] const ecs::EntityManager& world() const noexcept { return m_world; }

    /// The scene's own systems, for tests that want to count them.
    [[nodiscard]] const ecs::SystemManager& systems() const noexcept { return m_systems; }

    /// Whether the simulation is currently frozen by the pause control.
    ///
    /// Read by tests, and reported rather than assumed: the useful claim is not
    /// "the flag is true" but "the player did not move while it was true", and that
    /// needs the flag to be observable next to the world it stopped.
    [[nodiscard]] bool paused() const noexcept { return m_paused; }

    /// The three rendering toggles, on the scene's own render system.
    ///
    /// A forwarding accessor rather than a member, because the object that answers
    /// "are textures drawn" is the render system - it is where the answer is used -
    /// and [engine::systems::DebugRenderSystem] is given that same object to read.
    /// One state, one answer, two renderers that cannot disagree.
    [[nodiscard]] debug::DebugRenderState& debugRenderState() noexcept { return m_renderSystem.debugRenderState(); }
    [[nodiscard]] const debug::DebugRenderState& debugRenderState() const noexcept
    {
        return m_renderSystem.debugRenderState();
    }

    /// How tall the world this scene builds is, in cells.
    ///
    /// Read from the engine's configuration rather than hard-coded here, so the
    /// world size is a decision the composition root makes and the scene obeys. See
    /// [engine::level::LevelGrid] for why the height cannot come from the level
    /// file.
    [[nodiscard]] float cellsTall() const noexcept { return m_cellsTall; }

private:
    ecs::EntityManager m_world;
    ecs::SystemManager m_systems;

    /// Declared after the world and the systems, because a render pass reads the
    /// world and a render system borrows the renderer and the camera.
    engine::systems::RenderSystem m_renderSystem;

    /// Declared after [m_renderSystem](PlayScene.hpp) because it **borrows that
    /// object's** debug state, so the state has to exist before this is built. That
    /// is also what makes it safe on the way out: destruction is the reverse, so the
    /// debug renderer is destroyed first, while the state it points at is still
    /// alive.
    engine::systems::DebugRenderSystem m_debugRenderSystem;

    /// The empty action snapshot a render pass submits, because
    /// [engine::systems::RenderSystem] reads no input and a render pass has none to give.
    input::ActionState m_noActions;

    /// Whether the simulation is frozen, and the only thing pause is.
    ///
    /// A plain member of the scene, not a global and not a component, which is what
    /// makes "pause does not survive into the next level" a structural fact rather
    /// than something to remember: [engine::Application] destroys this object on
    /// every transition.
    ///
    /// Not part of [engine::debug::DebugRenderState], because it is not a rendering
    /// fact. It says whether the world is being simulated, which is a different
    /// question from what the world looks like - the render state is still read
    /// while paused, so the overlays can be switched on and off in a stopped game.
    bool m_paused = false;

    float m_cellsTall;
};

} // namespace engine::scene
