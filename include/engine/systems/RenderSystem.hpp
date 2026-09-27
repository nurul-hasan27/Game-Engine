#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/ecs/System.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/Renderer.hpp"

namespace engine::ecs
{
class EntityManager;
}

namespace engine::input
{
class Input;
}

namespace engine::systems
{

/// Draws every entity that has both a `components::Transform` and a
/// `components::Rectangle`.
///
/// The system is the whole point of the ECS: it reads component data, works out
/// where things belong, and asks the renderer to put them there. It stores
/// nothing, owns no entities, does not own the window, and loads no assets.
///
/// An entity missing either component is simply not drawn, and a dead entity is
/// skipped, because the query already filters both. There is no special case
/// here, and no error: "has no renderable component" is the normal way for an
/// entity to exist.
///
/// ### World in, screen out
///
/// This is the only system that knows a camera exists. It reads each entity's
/// **world** position straight from its `Transform` and hands the renderer a
/// **screen** position, with the conversion done by the pure function
/// `graphics::toRenderTransform()`.
///
/// The system performs no world to screen arithmetic of its own and has no idea
/// what a viewport is. It also does no physics, no gameplay and no culling, and
/// it never writes to a `Transform`.
///
/// The camera is held by **const reference**: the camera is owned by
/// `Application`, this system only reads it, and a render system that could
/// silently move the camera would be a very hard bug to find. See
/// [docs/camera.md](docs/camera.md).
///
/// It takes a `deltaSeconds` like any other system, and deliberately ignores it.
/// Drawing does not integrate over time, and accepting the parameter rather than
/// declaring a different signature keeps one uniform interface.
class RenderSystem final : public engine::ecs::System
{
public:
    /// The renderer, the camera and the assets are all referenced, not owned: the
    /// window, its context, the view onto the world and every loaded image belong
    /// to `Application`, and a system must never own the things it draws with.
    ///
    /// All three are **required**. There is deliberately no overload that omits the
    /// asset manager and no "assets unavailable" state: a render system that could
    /// exist without a way to resolve an asset name would have to decide what to
    /// do about a textured entity at draw time, and every answer to that is wrong
    /// somewhere - skipping it hides the problem, and failing mid-frame is a
    /// worse place to find out. `Application` owns the manager and passes it here,
    /// so a `RenderSystem` always has one.
    ///
    /// The manager is only read, to resolve a name into a handle. The system does
    /// not load, cache or keep one, and cannot add, replace or remove an asset:
    /// the interface it is handed through is const, so it has no way to.
    RenderSystem(graphics::Renderer& renderer, const graphics::Camera& camera,
                 const assets::AssetManager& assets) noexcept
        : m_renderer{&renderer}, m_camera{&camera}, m_assets{assets}
    {
    }

    /// Takes `input` only because every system shares one signature. Drawing
    /// does not read the keyboard, and this is not a way for rendering to grow a
    /// dependency on input later.
    void update(engine::ecs::EntityManager& entities, input::Input& input, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "RenderSystem"; }

private:
    graphics::Renderer* m_renderer = nullptr;
    const graphics::Camera* m_camera = nullptr;

    /// A reference, not a nullable pointer, because there is no valid empty state
    /// to represent: the manager is a construction requirement. A pointer here
    /// would be a member that can only ever be null by mistake, and every use
    /// would carry a null check for a case the type should have made impossible.
    ///
    /// Bound once in the constructor and never rebound, which is what lets the
    /// `Application` member order guarantee the manager outlives this system.
    const assets::AssetManager& m_assets;
};

} // namespace engine::systems
