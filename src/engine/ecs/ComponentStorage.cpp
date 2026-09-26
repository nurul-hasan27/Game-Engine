#include "engine/ecs/ComponentStorage.hpp"

#include <algorithm>

namespace engine::ecs
{

bool ComponentStorage::eraseType(const std::type_index type) noexcept
{
    const auto matchesType = [type](const std::unique_ptr<ComponentBase>& component)
    { return component->type() == type; };

    const auto position = std::find_if(m_components.begin(), m_components.end(), matchesType);

    if (position == m_components.end())
    {
        return false;
    }

    m_components.erase(position);
    return true;
}

ComponentBase* ComponentStorage::findBase(const std::type_index type) noexcept
{
    const auto matchesType = [type](const std::unique_ptr<ComponentBase>& component)
    { return component->type() == type; };

    const auto position = std::find_if(m_components.begin(), m_components.end(), matchesType);

    return position != m_components.end() ? position->get() : nullptr;
}

const ComponentBase* ComponentStorage::findBase(const std::type_index type) const noexcept
{
    const auto matchesType = [type](const std::unique_ptr<ComponentBase>& component)
    { return component->type() == type; };

    const auto position = std::find_if(m_components.begin(), m_components.end(), matchesType);

    return position != m_components.end() ? position->get() : nullptr;
}

} // namespace engine::ecs
