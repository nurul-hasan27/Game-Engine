#pragma once

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
    /// Both the renderer and the camera are referenced, not owned: the window,
    /// its context and the view onto the world all belong to `Application`, and a
    /// system must never own the things it draws with.
    RenderSystem(graphics::Renderer& renderer, const graphics::Camera& camera) noexcept
        : m_renderer{&renderer}, m_camera{&camera}
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
};

} // namespace engine::systems
