#include "engine/ecs/SystemManager.hpp"

#include "engine/ecs/EntityManager.hpp"
#include "engine/input/Input.hpp"

namespace engine::ecs
{

void SystemManager::update(EntityManager& entities, input::Input& input, const float deltaSeconds)
{
    // Registration order, deliberately, and nothing else. Each system receives
    // the world, the input state and the frame duration, and is the only thing
    // that decides what it touches. The same input object and the same delta go
    // to every system, so all of them agree on the frame.
    for (const std::unique_ptr<System>& system : m_systems)
    {
        system->update(entities, input, deltaSeconds);
    }
}

} // namespace engine::ecs
