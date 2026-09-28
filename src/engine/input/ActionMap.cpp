#include "engine/input/ActionMap.hpp"

namespace engine::input
{

void ActionMap::registerAction(const Action action, const Key key) noexcept
{
    if (!isValid(action))
    {
        return;
    }

    Bindings& bindings = m_bindings[toIndex(action)];

    // Registering the same key twice is a no-op rather than a duplicate, so a map
    // assembled from two places stays correct and the count stays meaningful.
    for (std::size_t index = 0U; index < bindings.count; ++index)
    {
        if (bindings.keys[index] == key)
        {
            return;
        }
    }

    // The table cannot grow, so a full action keeps the keys it has. That is a
    // bound rather than a failure: the action still works, through those keys.
    if (bindings.count >= kMaxBindingsPerAction)
    {
        return;
    }

    bindings.keys[bindings.count] = key;
    ++bindings.count;
}

void ActionMap::registerAction(const Action action, const Key first, const Key second) noexcept
{
    registerAction(action, first);
    registerAction(action, second);
}

void ActionMap::unregisterAction(const Action action) noexcept
{
    if (isValid(action))
    {
        m_bindings[toIndex(action)] = Bindings{};
    }
}

std::size_t ActionMap::bindingCount(const Action action) const noexcept
{
    return isValid(action) ? m_bindings[toIndex(action)].count : 0U;
}

Key ActionMap::keyAt(const Action action, const std::size_t index) const noexcept
{
    if (!isValid(action))
    {
        return Key::Unknown;
    }

    const Bindings& bindings = m_bindings[toIndex(action)];

    return index < bindings.count ? bindings.keys[index] : Key::Unknown;
}

bool ActionMap::isBound(const Action action) const noexcept
{
    for (std::size_t index = 0U; index < bindingCount(action); ++index)
    {
        if (isTrackable(keyAt(action, index)))
        {
            return true;
        }
    }

    return false;
}

bool ActionMap::hasAnyBinding() const noexcept
{
    for (std::size_t index = 0U; index < kActionCount; ++index)
    {
        if (isBound(static_cast<Action>(index)))
        {
            return true;
        }
    }

    return false;
}

void ActionMap::clear() noexcept
{
    for (Bindings& bindings : m_bindings)
    {
        bindings = Bindings{};
    }
}

ActionMap ActionMap::withDefaultBindings() noexcept
{
    return defaultActionMap();
}

ActionMap defaultActionMap() noexcept
{
    ActionMap map;

    // Movement. The letter is Assignment 3's control and the arrow is the camera
    // demo's, and the two are one action rather than two - see the header.
    map.registerAction(Action::MoveLeft, Key::A, Key::Left);
    map.registerAction(Action::MoveRight, Key::D, Key::Right);
    map.registerAction(Action::MoveUp, Key::W, Key::Up);
    map.registerAction(Action::MoveDown, Key::S, Key::Down);

    // Assignment 3's player controls. W appears twice on purpose: it is Jump
    // because the assignment says so, and MoveUp because the demo has always used
    // it. Both are kept.
    map.registerAction(Action::Jump, Key::W);
    map.registerAction(Action::Shoot, Key::Space);

    // Assignment 3's debugging toggles and pause.
    map.registerAction(Action::Pause, Key::P);
    map.registerAction(Action::ToggleTextures, Key::T);
    map.registerAction(Action::ToggleBoundingBoxes, Key::C);
    map.registerAction(Action::ToggleGrid, Key::G);

    // The camera demo's zoom, which is a per-press action.
    map.registerAction(Action::ZoomIn, Key::X);
    map.registerAction(Action::ZoomOut, Key::Z);

    // Assignment 3's Escape. Nothing in the engine acts on this; it is the game's
    // policy to decide, which Application's own documentation already insists on.
    map.registerAction(Action::Quit, Key::Escape);

    return map;
}

} // namespace engine::input
