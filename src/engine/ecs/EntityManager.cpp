#include "engine/ecs/EntityManager.hpp"

#include <algorithm>
#include <stdexcept>

namespace engine::ecs
{

Entity& EntityManager::addEntity(std::string tag)
{
    const EntityId id = m_nextId++;
    m_entities.emplace_back(id, std::move(tag), Entity::ManagerAccess{});

    return m_entities.back();
}

EntityView EntityManager::getEntities() const
{
    return EntityView{&m_entities, std::string_view{}};
}

EntityView EntityManager::getEntities(const std::string_view tag) const
{
    return EntityView{&m_entities, tag};
}

void EntityManager::destroyEntity(const Entity& entity) const
{
    if (!entity.isAlive())
    {
        throw std::logic_error{"destroyEntity called on an entity that is already dead"};
    }

    entity.markDead();
}

void EntityManager::update()
{
    const auto isDead = [](const Entity& entity) { return !entity.isAlive(); };
    m_entities.erase(std::remove_if(m_entities.begin(), m_entities.end(), isDead), m_entities.end());
}

std::size_t EntityManager::aliveEntityCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(m_entities.begin(), m_entities.end(),
                                                   [](const Entity& entity) { return entity.isAlive(); }));
}

} // namespace engine::ecs
