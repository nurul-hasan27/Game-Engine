#pragma once

#include "engine/ecs/System.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace engine::ecs
{

class EntityManager;

/// Owns a set of systems and runs them in a deterministic order.
///
/// Ownership is one-directional: the SystemManager owns systems, and systems own
/// nothing. Entities always belong to the EntityManager, never to a system and
/// never to the SystemManager, so tearing down the SystemManager cannot affect
/// the world.
///
/// Execution order is registration order, and nothing else. There is no sorting,
/// no priority and no dependency graph: a system runs where it was added, so
/// "input, then movement, then physics" is expressed by adding them in that
/// order and the result is the same on every run and every platform.
class SystemManager
{
public:
    SystemManager() = default;
    ~SystemManager() = default;

    SystemManager(const SystemManager&) = delete;
    SystemManager& operator=(const SystemManager&) = delete;
    SystemManager(SystemManager&&) = delete;
    SystemManager& operator=(SystemManager&&) = delete;

    /// Constructs a system of type S in place and appends it to the run order.
    ///
    /// Returns a reference to it, which stays valid as further systems are added
    /// because each system is separately allocated.
    template <typename S, typename... Args>
    S& add(Args&&... args)
    {
        auto system = std::make_unique<S>(std::forward<Args>(args)...);
        S& reference = *system;
        m_systems.push_back(std::move(system));

        return reference;
    }

    [[nodiscard]] const System& systemAt(const std::size_t index) const { return *m_systems.at(index); }

    [[nodiscard]] std::size_t systemCount() const noexcept { return m_systems.size(); }

    /// Runs every registered system once, in registration order.
    ///
    /// @param entities The world to hand to each system. Owned by the caller.
    /// @param input Keyboard state for this frame, handed to each system by
    ///        reference. The same state object goes to every system, so they all
    ///        agree on what the player is doing.
    /// @param deltaSeconds Seconds elapsed since the previous frame. Passed
    ///        unchanged to every system, so all of them see the same value for
    ///        the same frame. Measured by engine::Time; see System for why the
    ///        delta is a parameter rather than something systems look up.
    void update(EntityManager& entities, const input::ActionState& actions, float deltaSeconds);

private:
    // unique_ptr so that adding a system does not move the systems already
    // registered, which would invalidate references handed out by add().
    std::vector<std::unique_ptr<System>> m_systems;
};

} // namespace engine::ecs
