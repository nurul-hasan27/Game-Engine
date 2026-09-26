#include "engine/systems/RenderSystem.hpp"

#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"

namespace engine::systems
{

void RenderSystem::update(engine::ecs::EntityManager& entities, const float deltaSeconds)
{
    // Drawing is not time dependent, but the parameter is accepted so every
    // system has one uniform signature.
    static_cast<void>(deltaSeconds);

    for (auto&& [entity, transform, rectangle] :
         entities.query<components::Transform, components::Rectangle>())
    {
        static_cast<void>(entity);
        m_renderer->drawRectangle(rectangle.size, rectangle.color, graphics::toRenderTransform(transform));
    }
}

} // namespace engine::systems
