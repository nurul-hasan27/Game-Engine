#include "engine/input/ActionState.hpp"

namespace engine::input
{

void ActionState::update(const ActionMap& map, const Input& input) noexcept
{
    // Cleared first and unconditionally, so a field can never keep a value from a
    // previous frame. Every slot is written exactly once below, including the
    // "unbound" case, so there is no path that leaves a stale edge behind.
    m_active.fill(false);
    m_pressed.fill(false);
    m_released.fill(false);

    for (std::size_t actionIndex = 0U; actionIndex < kActionCount; ++actionIndex)
    {
        const Action action = static_cast<Action>(actionIndex);

        const std::size_t bindingCount = map.bindingCount(action);
        for (std::size_t binding = 0U; binding < bindingCount; ++binding)
        {
            const Key key = map.keyAt(action, binding);

            // An untrackable key is inert. `isKeyDown` and friends already return
            // false for one, so this is belt and braces around the three
            // implementations rather than a filter: the alternative is reading a
            // slot the key enum says is not there.
            if (!isTrackable(key))
            {
                continue;
            }

            m_active[actionIndex] = m_active[actionIndex] || input.isKeyDown(key);
            m_pressed[actionIndex] = m_pressed[actionIndex] || input.isKeyPressed(key);
            m_released[actionIndex] = m_released[actionIndex] || input.isKeyReleased(key);
        }
    }
}

void ActionState::reset() noexcept
{
    m_active.fill(false);
    m_pressed.fill(false);
    m_released.fill(false);
}

bool ActionState::anyActionActive() const noexcept
{
    for (const bool active : m_active)
    {
        if (active)
        {
            return true;
        }
    }

    return false;
}

} // namespace engine::input
