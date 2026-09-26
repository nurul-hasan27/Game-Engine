#pragma once

#include "engine/ecs/Entity.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <tuple>
#include <type_traits>
#include <utility>

namespace engine::ecs
{

/// Compile-time check that every type in a pack appears only once.
///
/// C++17 has no is_unique over a parameter pack, so this is spelled out.
template <typename... Ts>
struct HasDuplicateTypes;

template <typename T>
struct HasDuplicateTypes<T> : std::false_type
{
};

template <typename T, typename... Rest>
struct HasDuplicateTypes<T, Rest...>
    : std::bool_constant<(std::is_same_v<T, Rest> || ...) || HasDuplicateTypes<Rest...>::value>
{
};

/// A non-owning, filtered view over the entities of an EntityManager that hold
/// every one of the requested component types.
///
/// This is what a system iterates:
///
/// ```cpp
/// for (auto&& [entity, position] : entities.query<Position>())
/// {
///     position.value += velocityOf(entity);
/// }
///
/// for (auto&& [entity, position, velocity] : entities.query<Position, Velocity>())
/// {
///     position.value += velocity.value;
/// }
/// ```
///
/// Dereferencing yields a `std::tuple` of the entity followed by a reference to
/// each requested component, so the result works with structured bindings and
/// no entity or component is ever copied.
///
/// ### What a query owns
///
/// Nothing. A query holds a pointer to the manager's entity storage and
/// borrows it. No collection is materialised, nothing is allocated, and no
/// entity or component is duplicated: iteration walks the manager's storage
/// once, visiting each qualifying entity exactly once.
///
/// ### Lifetime
///
/// The query borrows from the EntityManager it was created from, so it is valid
/// only while that manager's entity set is unchanged. See the "Structural
/// mutation" note below and docs/ecs.md.
///
/// ### Structural mutation
///
/// Writing to component *data* during iteration is fully supported: the
/// components are separately allocated, so writing through them never moves
/// anything.
///
/// Structural changes are **not** supported during iteration, because a query
/// holds references into the manager's own containers:
///
/// - `EntityManager::addEntity()` or `EntityManager::update()` — may reallocate
///   or move entities, invalidating the iterator and every reference it yields.
/// - `addComponent()` / `removeComponent()` on a *matching* entity — removing
///   the very component being iterated dangles that reference, and adding one
///   changes the entity's match set mid-walk.
/// - Destroying an entity is fine, because `destroyEntity()` only sets a flag.
///   The actual erase happens in `update()`, which is the forbidden call.
///
/// The rule of thumb: a system may write component values freely, and must
/// collect anything structural and apply it after the loop.
///
/// ### Constness
///
/// `IsConst` selects the access level. A query obtained from a const
/// EntityManager yields `const` component references, so a read-only world
/// cannot be mutated through it. A query obtained from a non-const manager
/// yields mutable references, because a system's whole job is to write data.
/// This does not weaken Entity's own constness rules, which continue to mean
/// "identity is fixed, not data".
template <bool IsConst, typename... Ts>
class Query
{
    static_assert(sizeof...(Ts) > 0, "query<> requires at least one component type");
    static_assert(!HasDuplicateTypes<Ts...>::value,
                  "query<T, T> is not allowed: listing a component type twice would yield two "
                  "references to the same component. Write query<T> instead.");

public:
    /// The entity storage a query borrows. A read-only query borrows it as
    /// const, so a const EntityManager cannot hand out a mutable view without a
    /// const_cast anywhere.
    using Storage = std::conditional_t<IsConst, const std::deque<Entity>, std::deque<Entity>>;

    /// The entity a query yields: const for a read-only query, mutable otherwise.
    using EntityRef = std::conditional_t<IsConst, const Entity&, Entity&>;

    /// A reference to each requested component: `const T&` for a read-only
    /// query, `T&` for a mutable one.
    template <typename T>
    using ComponentRef = std::conditional_t<IsConst, const T, T>&;

    /// The tuple a query yields: the entity, then each requested component.
    using Reference = std::tuple<EntityRef, ComponentRef<Ts>...>;

    class Iterator
    {
    public:
        // Note: Storage::iterator would not do here. A const deque still has a
        // mutable `iterator` typedef, so constness has to be selected explicitly.
        using EntityIterator =
            std::conditional_t<IsConst, std::deque<Entity>::const_iterator, std::deque<Entity>::iterator>;
        using iterator_category = std::forward_iterator_tag;
        using value_type = Reference;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Reference;

        Iterator() = default;

        Iterator(EntityIterator current, EntityIterator last) noexcept : m_current{current}, m_last{last}
        {
            skipNonMatching();
        }

        [[nodiscard]] Reference operator*() const
        {
            EntityRef entity = *m_current;

            // The query has already established that every requested component
            // is present, so these pointers are non-null.
            if constexpr (IsConst)
            {
                return Reference{entity, *entity.template tryGetConstComponent<Ts>()...};
            }
            else
            {
                return Reference{entity, *entity.template tryGetComponent<Ts>()...};
            }
        }

        Iterator& operator++()
        {
            ++m_current;
            skipNonMatching();
            return *this;
        }

        Iterator operator++(int)
        {
            Iterator previous{*this};
            ++*this;
            return previous;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept
        {
            return m_current == other.m_current;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept { return !(*this == other); }

    private:
        void skipNonMatching()
        {
            while (m_current != m_last && !matches(*m_current))
            {
                ++m_current;
            }
        }

        /// An entity qualifies when it is alive and holds every requested
        /// component. Folding the presence checks together avoids re-walking for
        /// each type once a check has already failed.
        [[nodiscard]] bool matches(const Entity& entity) const
        {
            return entity.isAlive() && (... && hasComponent<Ts>(entity));
        }

        template <typename T>
        [[nodiscard]] static bool hasComponent(const Entity& entity)
        {
            if constexpr (IsConst)
            {
                return entity.tryGetConstComponent<T>() != nullptr;
            }
            else
            {
                return entity.tryGetComponent<T>() != nullptr;
            }
        }

        EntityIterator m_current{};
        EntityIterator m_last{};
    };

    [[nodiscard]] Iterator begin() const { return Iterator{m_entities->begin(), m_entities->end()}; }

    [[nodiscard]] Iterator end() const { return Iterator{m_entities->end(), m_entities->end()}; }

    /// Number of qualifying entities. Walks the range, so this is O(entities).
    [[nodiscard]] std::size_t size() const
    {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto& entry : *this)
        {
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool empty() const { return begin() == end(); }

private:
    friend class EntityManager;

    explicit Query(Storage* entities) noexcept : m_entities{entities} {}

    Storage* m_entities = nullptr;
};

/// A mutable query: yields writable component references.
template <typename... Ts>
using MutableQuery = Query<false, Ts...>;

/// A read-only query: yields const component references.
template <typename... Ts>
using ConstQuery = Query<true, Ts...>;

} // namespace engine::ecs
