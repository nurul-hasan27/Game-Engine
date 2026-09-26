#include "engine/ecs/SystemManager.hpp"

#include "engine/ecs/EntityManager.hpp"

namespace engine::ecs
{

void SystemManager::update(EntityManager& entities, const float deltaSeconds)
{
    // Registration order, deliberately, and nothing else. Each system receives
    // the manager and is the only thing that decides what it touches. The same
    // delta goes to every system, unchanged, so all of them agree on how long
    // this frame was.
    for (const std::unique_ptr<System>& system : m_systems)
    {
        system->update(entities, deltaSeconds);
    }
}

} // namespace engine::ecs
