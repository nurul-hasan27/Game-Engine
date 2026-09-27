#include "engine/systems/RenderSystem.hpp"

#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"

namespace engine::systems
{

void RenderSystem::update(engine::ecs::EntityManager& entities, input::Input& input, const float deltaSeconds)
{
    // Drawing is not time dependent and does not read the keyboard. Both
    // parameters are accepted only so every system shares one signature.
    static_cast<void>(input);
    static_cast<void>(deltaSeconds);

    for (auto&& [entity, transform, rectangle] :
         entities.query<components::Transform, components::Rectangle>())
    {
        static_cast<void>(entity);
        // The one line where world space becomes screen space. The entity's
        // Transform is read, never written: it stays a world position however
        // the camera is configured.
        m_renderer->drawRectangle(rectangle.size, rectangle.color, graphics::toRenderTransform(transform, *m_camera));
    }
}

} // namespace engine::systems
