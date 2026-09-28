#include "engine/ecs/SystemManager.hpp"

#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"

namespace engine::ecs
{

void SystemManager::update(EntityManager& entities, const input::ActionState& actions, const float deltaSeconds)
{
    // Registration order, deliberately, and nothing else. Each system receives the
    // world, this frame's action snapshot and the frame duration, and is the only
    // thing that decides what it touches. The same snapshot and the same delta go
    // to every system, so all of them agree on the frame - which is the reason
    // the snapshot is computed once per frame rather than per system.
    for (const std::unique_ptr<System>& system : m_systems)
    {
        system->update(entities, actions, deltaSeconds);
    }
}

} // namespace engine::ecs
