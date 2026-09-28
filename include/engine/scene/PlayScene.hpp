#pragma once

#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/scene/Scene.hpp"
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

    /// The empty action snapshot a render pass submits, because
    /// [engine::systems::RenderSystem] reads no input and a render pass has none to give.
    input::ActionState m_noActions;

    float m_cellsTall;
};

} // namespace engine::scene
