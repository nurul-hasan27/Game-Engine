#pragma once

#include "engine/ecs/Entity.hpp"

#include <cstddef>
#include <deque>
#include <iterator>
#include <string_view>

namespace engine::ecs
{

class EntityManager;

/// A non-owning view over the entities of one EntityManager that are currently
/// alive and, optionally, carry a given tag.
///
/// The view owns nothing and allocates nothing. It is built on demand from the
/// manager, so it is cheap to create inside a frame loop. It deliberately does
/// *not* expose the manager's internal container, which keeps callers from
/// breaking the ownership and liveness rules.
///
/// The view yields `const Entity&`. That is the right level of access for
/// iteration: a system can read identity, tags and components, and can request
/// destruction, but cannot resurrect a dead entity or invent a new id.
///
/// ### Lifetime
///
/// The view is only valid while the manager's entity set is unchanged. Any call
/// to EntityManager::addEntity() or EntityManager::update() invalidates it, and
/// any Entity reference or component reference obtained through it.
///
/// Requesting destruction during iteration is explicitly allowed: it only sets a
/// flag, so iteration continues safely. This is the whole point of deferring
/// destruction.
class EntityView
{
public:
    /// Forward iterator that skips dead entities and entities whose tag does
    /// not match the view's filter.
    class Iterator
    {
    public:
        using EntityIterator = std::deque<Entity>::const_iterator;
        using iterator_category = std::forward_iterator_tag;
        using value_type = const Entity;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entity*;
        using reference = const Entity&;

        Iterator() = default;

        Iterator(EntityIterator current, EntityIterator last, std::string_view tag) noexcept
            : m_current{current}, m_last{last}, m_tag{tag}
        {
            skipExcluded();
        }

        [[nodiscard]] reference operator*() const noexcept { return *m_current; }
        [[nodiscard]] pointer operator->() const noexcept { return &*m_current; }

        Iterator& operator++()
        {
            ++m_current;
            skipExcluded();
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
        void skipExcluded()
        {
            while (m_current != m_last && !matches(*m_current))
            {
                ++m_current;
            }
        }

        [[nodiscard]] bool matches(const Entity& entity) const
        {
            // An empty tag means "any tag", which is how getEntities() and
            // getEntities(tag) share one implementation.
            return entity.isAlive() && (m_tag.empty() || entity.tag() == m_tag);
        }

        EntityIterator m_current{};
        EntityIterator m_last{};
        std::string_view m_tag{};
    };

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

    /// Returns true if the view contains no entities. Walks the range.
    [[nodiscard]] bool empty() const { return begin() == end(); }

private:
    friend class EntityManager;

    /// An empty `tag` matches every tag.
    EntityView(const std::deque<Entity>* entities, std::string_view tag) noexcept
        : m_entities{entities}, m_tag{tag}
    {
    }

    const std::deque<Entity>* m_entities = nullptr;
    std::string_view m_tag{};
};

} // namespace engine::ecs
