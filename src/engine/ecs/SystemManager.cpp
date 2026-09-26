#include "engine/ecs/SystemManager.hpp"

#include "engine/ecs/EntityManager.hpp"

namespace engine::ecs
{

void SystemManager::update(EntityManager& entities)
{
    // Registration order, deliberately, and nothing else. Each system receives
    // the manager and is the only thing that decides what it touches.
    for (const std::unique_ptr<System>& system : m_systems)
    {
        system->update(entities);
    }
}

} // namespace engine::ecs
