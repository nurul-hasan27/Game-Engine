#pragma once

#include "engine/ecs/Entity.hpp"
#include "engine/ecs/EntityView.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace engine::ecs
{

/// Owns every entity in the world and decides their lifetime.
///
/// The manager is the only way to create an entity, and the only way to
/// request that one be destroyed. Systems never own entities; they borrow views
/// of the manager's entities for the length of a frame.
///
/// ### Deferred destruction
///
/// destroyEntity() only flags an entity as dead. It never erases anything, so it
/// is safe to call while a system is iterating a view: iteration simply skips
/// flagged entities. update() then performs the actual cleanup. This is what
/// lets a system say "this entity must die" without invalidating the iteration
/// it is currently in.
///
/// ### Lifetime
///
/// Entity references and EntityView instances are invalidated by addEntity() and
/// by update(). Both are documented where they are handed out; the short version
/// is that a view may be held only for the duration of one system's pass.
class EntityManager
{
public:
    EntityManager() = default;
    ~EntityManager() = default;

    EntityManager(const EntityManager&) = delete;
    EntityManager& operator=(const EntityManager&) = delete;
    EntityManager(EntityManager&&) = delete;
    EntityManager& operator=(EntityManager&&) = delete;

    /// Creates a new entity with the given tag and returns a reference to it.
    ///
    /// The reference stays valid until the next addEntity() or update(). Ids are
    /// assigned here and are never reused, so an id always identifies exactly
    /// one entity for the lifetime of the process.
    ///
    /// Not [[nodiscard]]: spawning an entity for its effect on the world, rather
    /// than to keep a reference, is a normal thing to do.
    Entity& addEntity(std::string tag);

    /// All entities that are currently alive, in creation order.
    ///
    /// The returned view is a lightweight non-owning handle; see EntityView for
    /// its lifetime rules.
    [[nodiscard]] EntityView getEntities() const;

    /// All entities that are currently alive and carry `tag`.
    ///
    /// The `tag` must outlive the returned view. An empty tag matches every
    /// entity, which is what lets getEntities() and getEntities(tag) share one
    /// implementation.
    [[nodiscard]] EntityView getEntities(std::string_view tag) const;

    /// Requests destruction of `entity`. The entity is flagged dead immediately
    /// and stops appearing in views, but is only erased by update().
    ///
    /// Throws std::logic_error if the entity is already dead, so that double
    /// destruction is reported instead of silently ignored.
    void destroyEntity(const Entity& entity) const;

    /// Performs deferred cleanup, erasing every entity previously flagged by
    /// destroyEntity(). Invalidates all outstanding entity references and views.
    ///
    /// Systems do not exist yet, so this is the whole of the per-frame work.
    void update();

    /// Number of entities that are alive right now.
    [[nodiscard]] std::size_t aliveEntityCount() const noexcept;

    /// Number of entities stored, including any awaiting cleanup. Equal to
    /// aliveEntityCount() between updates, and useful for observing that
    /// destruction really was deferred.
    [[nodiscard]] std::size_t storedEntityCount() const noexcept { return m_entities.size(); }

private:
    // A deque rather than a vector on purpose: inserting at the end of a deque
    // never invalidates references to existing elements, so adding an entity
    // does not invalidate the entity references systems are holding. Erase()
    // during update() does invalidate them, which is why cleanup is a distinct,
    // documented step.
    std::deque<Entity> m_entities;

    EntityId m_nextId = kInvalidEntityId + 1;
};

} // namespace engine::ecs
