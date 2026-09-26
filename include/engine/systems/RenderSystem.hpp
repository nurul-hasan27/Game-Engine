#pragma once

#include "engine/ecs/System.hpp"
#include "engine/graphics/Renderer.hpp"

namespace engine::ecs
{
class EntityManager;
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
/// It takes a `deltaSeconds` like any other system, and deliberately ignores it.
/// Drawing does not integrate over time, and accepting the parameter rather than
/// declaring a different signature keeps one uniform interface.
class RenderSystem final : public engine::ecs::System
{
public:
    /// The renderer is referenced, not owned: the window and its context belong
    /// to `Application`, and a system must never own the thing it draws into.
    explicit RenderSystem(graphics::Renderer& renderer) noexcept : m_renderer{&renderer} {}

    void update(engine::ecs::EntityManager& entities, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "RenderSystem"; }

private:
    graphics::Renderer* m_renderer = nullptr;
};

} // namespace engine::systems
