#pragma once

#include "engine/ecs/ComponentStorage.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>

namespace engine::ecs
{

/// Stable identifier for an entity, unique among all entities the process has
/// ever created.
///
/// This is a plain integer rather than a pointer or an address, so it is cheap
/// to copy, stays valid for the entity's whole lifetime, and is directly
/// printable and usable as a map key when debugging.
using EntityId = std::uint64_t;

/// Reserved id meaning "no entity". Real ids start at 1.
inline constexpr EntityId kInvalidEntityId = 0;

class EntityManager;

/// An entity: an identity, an optional tag, a live/dead flag, and the
/// components attached to it.
///
/// An entity deliberately contains no behaviour. It cannot move, render,
/// attack, collide or take damage, because all of that belongs to systems in a
/// later phase. An entity is only an identity plus its data.
///
/// ### Ownership
///
/// An entity is owned by exactly one EntityManager and cannot be created
/// outside of one: its constructor requires a ManagerAccess token that only
/// EntityManager can produce. Entities are non-copyable, so two owners can
/// never end up sharing one identity. An entity owns its components outright
/// and destroys them with itself.
///
/// ### Constness
///
/// A `const Entity` means "this entity's identity is not changing". It does not
/// freeze its data: every component operation, including addComponent() and
/// removeComponent(), is callable on a const entity and mutates through it. That
/// is the usual ECS "logical const" arrangement, and it is what lets a system
/// walk a read-only entity view and still create, change and destroy components.
/// Only the id, the tag and liveness are protected by const, and they are
/// changed only by EntityManager.
class Entity
{
public:
    /// Proof that the caller is allowed to construct an Entity.
    ///
    /// Only EntityManager can create one, which keeps entities exclusively
    /// manager-owned even though the Entity constructor itself must be public
    /// for std::deque to construct elements in place.
    class ManagerAccess
    {
    private:
        friend class EntityManager;

        constexpr ManagerAccess() noexcept = default;
    };

    /// Creates an entity owned by a manager. Not callable outside the engine:
    /// ManagerAccess cannot be constructed by anyone but EntityManager.
    ///
    /// The constructor itself has to be public because std::deque constructs
    /// elements in place from inside its own code, which cannot be granted
    /// friendship. The ManagerAccess token is what actually enforces the
    /// restriction, and it is the reason this is safe.
    Entity(const EntityId id, std::string tag, ManagerAccess) noexcept
        : m_id{id}, m_tag{std::move(tag)}, m_isAlive{true}, m_components{}
    {
    }

    /// Entities are uniquely owned, so they are movable but not copyable.
    Entity(Entity&&) = default;
    Entity& operator=(Entity&&) = default;
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    ~Entity() = default;

    [[nodiscard]] EntityId id() const noexcept { return m_id; }

    [[nodiscard]] const std::string& tag() const noexcept { return m_tag; }

    /// False once destruction has been requested. The entity and its components
    /// stay intact and readable until the owning manager cleans up.
    [[nodiscard]] bool isAlive() const noexcept { return m_isAlive; }

    [[nodiscard]] std::size_t componentCount() const noexcept { return m_components.size(); }

    // -- Components -----------------------------------------------------------

    /// Adds a component of type T, constructed from `args`, and returns a
    /// reference to it.
    ///
    /// Throws std::logic_error if this entity already has a component of type T.
    ///
    /// Const for the same reason getComponent() is: a system walking a read-only
    /// entity view must be able to change what an entity is made of. Only the
    /// identity and liveness of an entity are protected by const.
    template <typename T, typename... Args>
    T& addComponent(Args&&... args) const
    {
        return m_components.emplace<T>(std::forward<Args>(args)...);
    }

    /// Returns true if this entity has a component of type T. Never throws and
    /// never creates the component.
    template <typename T>
    [[nodiscard]] bool hasComponent() const noexcept
    {
        return m_components.contains<T>();
    }

    /// Returns a reference to the component of type T.
    ///
    /// Throws std::logic_error if the component is absent. Absence is a
    /// programming error, so this reports it loudly rather than quietly handing
    /// back a default-constructed value that would then be overwritten, losing
    /// data. Use hasComponent() to test first, or tryGetComponent() when absence
    /// is an expected outcome.
    ///
    /// Callable on a const Entity and returning a mutable T&, see the note on
    /// constness in the class documentation. That is what allows a system to
    /// walk a read-only entity view and still write component data. It is a const
    /// member function precisely so that it can be.
    template <typename T>
    [[nodiscard]] T& getComponent() const
    {
        if (T* const component = m_components.find<T>())
        {
            return *component;
        }

        throw std::logic_error{missingComponentMessage<T>()};
    }

    /// Returns a pointer to the component of type T, or nullptr if absent.
    ///
    /// Returns T* rather than const T* for the same reason getComponent()
    /// returns T*.
    template <typename T>
    [[nodiscard]] T* tryGetComponent() const noexcept
    {
        return m_components.find<T>();
    }

    /// Removes the component of type T.
    ///
    /// Throws std::logic_error if the component is absent, because removing
    /// something that was never there usually means a caller has lost track of
    /// the entity's composition. Use tryRemoveComponent() when absence is fine.
    template <typename T>
    void removeComponent() const
    {
        if (!m_components.erase<T>())
        {
            throw std::logic_error{"removeComponent called for a component this entity does not have"};
        }
    }

    /// Removes the component of type T, returning false if there was none.
    template <typename T>
    [[nodiscard]] bool tryRemoveComponent() const noexcept
    {
        return m_components.erase<T>();
    }

private:
    friend class EntityManager;

    /// Flags the entity as dead. Logically const because the entity's identity
    /// does not change, only its liveness, and only the owning manager may do
    /// this.
    void markDead() const noexcept { m_isAlive = false; }

    template <typename T>
    [[nodiscard]] std::string missingComponentMessage() const
    {
        return "entity " + std::to_string(m_id) + " (" + m_tag + ") has no component of type " + typeid(T).name();
    }

    EntityId m_id = kInvalidEntityId;
    std::string m_tag;
    mutable bool m_isAlive = true;

    /// Mutable because component *values* are deliberately writable through a
    /// const Entity: identity is what const protects, not data. See the note on
    /// constness above.
    mutable ComponentStorage m_components;
};

} // namespace engine::ecs
