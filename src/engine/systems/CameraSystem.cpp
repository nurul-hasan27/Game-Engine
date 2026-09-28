#include "engine/systems/CameraSystem.hpp"

#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/EntityView.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::systems
{

void CameraSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                          const float deltaSeconds)
{
    // Following a target is a consequence of where it is this frame, not of what
    // the player is pressing and not of how long the frame was. Both parameters
    // exist only because every system shares one signature.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    // Resolved fresh every frame. See the class documentation: an Entity& is
    // borrowed from the manager and EntityManager::update() invalidates it, so
    // caching one here would be a dangling reference waiting for the first
    // entity to be destroyed.
    //
    // The view yields const Entity&, so the target's Transform is only readable
    // here, which is exactly the guarantee this system needs: it cannot write to
    // the target even by accident.
    const engine::ecs::EntityView targets = entities.getEntities(m_targetTag);

    for (const engine::ecs::Entity& target : targets)
    {
        const components::Transform* const transform = target.tryGetConstComponent<components::Transform>();

        if (transform == nullptr)
        {
            // The tag exists but the entity is not something that can be
            // followed. Treat it as no target rather than dereferencing nothing.
            return;
        }

        m_camera->setPosition(transform->position);
        return; // first match only: a duplicate tag must not fight itself
    }

    // No live target. The camera keeps the position it had. Snapping to the
    // origin instead would make the world jump for no reason, and this is not an
    // error: a target that has not spawned yet is entirely normal.
}

} // namespace engine::systems
