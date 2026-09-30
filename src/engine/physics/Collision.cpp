#include "engine/physics/Collision.hpp"

#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"

#include <optional>

namespace engine::physics
{

using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::kInvalidEntityId;

namespace
{

/// The box an entity occupies, or nothing if it has none to offer.
///
/// Absent means any of: the entity is not alive, it has no
/// [engine::components::Transform], or it has no [engine::components::Collider].
/// A zero-size collider is **not** absent - it is a degenerate point, and
/// [Aabb] handles it as one, reporting no overlap with anything.
///
/// Read through `tryGetConstComponent` rather than `getComponent`, because a missing
/// component is an expected answer here and `getComponent` throws.
[[nodiscard]] std::optional<Aabb> boxOf(const Entity& entity) noexcept
{
    if (!entity.isAlive())
    {
        return std::nullopt;
    }

    const components::Transform* const transform = entity.tryGetConstComponent<components::Transform>();
    const components::Collider* const collider = entity.tryGetConstComponent<components::Collider>();
    if (transform == nullptr || collider == nullptr)
    {
        return std::nullopt;
    }

    return Aabb{transform->position, collider->size};
}

/// The box an entity occupied before the current physics step.
[[nodiscard]] std::optional<Aabb> previousBoxOf(const Entity& entity) noexcept
{
    if (!entity.isAlive())
    {
        return std::nullopt;
    }

    const components::Transform* const transform = entity.tryGetConstComponent<components::Transform>();
    const components::Collider* const collider = entity.tryGetConstComponent<components::Collider>();
    if (transform == nullptr || collider == nullptr)
    {
        return std::nullopt;
    }

    return Aabb{transform->prevPosition, collider->size};
}

/// The shared body of [getOverlap] and [getPreviousOverlap]: the two boxes, or nothing.
///
/// Both public functions are this plus a choice of which position to read, which is
/// the only thing that differs between them. Writing the absent handling once means
/// the two cannot come to disagree about what an absent box means - and they must
/// agree, because a consumer comparing the two is exactly how [landedOn] works, and a
/// disagreement would show up as a landing that never resolves.
[[nodiscard]] std::optional<std::pair<Aabb, Aabb>> boxesOf(const Entity& first, const Entity& second) noexcept
{
    const std::optional<Aabb> firstBox = boxOf(first);
    const std::optional<Aabb> secondBox = boxOf(second);
    if (!firstBox.has_value() || !secondBox.has_value())
    {
        return std::nullopt;
    }

    return std::pair<Aabb, Aabb>{*firstBox, *secondBox};
}

} // namespace

bool isParticipant(const Collision& collision, const EntityId entity) noexcept
{
    return collision.first == entity || collision.second == entity;
}

EntityId partnerOf(const Collision& collision, const EntityId entity) noexcept
{
    if (collision.first == entity)
    {
        return collision.second;
    }

    if (collision.second == entity)
    {
        return collision.first;
    }

    return kInvalidEntityId;
}

Vec2 resolutionFor(const Collision& collision, const EntityId entity) noexcept
{
    // [Collision::resolution] is oriented towards `first`, so a consumer that is
    // `second` sees the opposite push. Negating rather than re-deriving keeps the two
    // in step by construction: the physics system computes the vector once and this
    // function only decides whose point of view to read it from.
    if (collision.first == entity)
    {
        return collision.resolution;
    }

    return -collision.resolution;
}

bool hasStaticPartner(const Collision& collision, const EntityId entity) noexcept
{
    if (collision.first == entity)
    {
        return collision.secondBody == BodyType::Static;
    }

    if (collision.second == entity)
    {
        return collision.firstBody == BodyType::Static;
    }

    return false;
}

bool landedOn(const Collision& collision, const EntityId entity) noexcept
{
    if (!isParticipant(collision, entity))
    {
        return false;
    }

    // Arrived vertically: was beside the partner, was not vertically overlapping it,
    // and is now. The three clauses together are what separate "fell onto it" from
    // "was already touching it" and from "walked into its side".
    const bool arrivedFromOutside = collision.previousOverlap.x > 0.0F && collision.previousOverlap.y <= 0.0F;
    const bool nowPenetratingVertically = collision.overlap.y > 0.0F;

    // And the push was upward, which is what says the player came from above rather
    // than from below. Negative y is up in this engine.
    const bool pushedUp = resolutionFor(collision, entity).y < 0.0F;

    return arrivedFromOutside && nowPenetratingVertically && pushedUp && hasStaticPartner(collision, entity);
}

bool hitCeilingWith(const Collision& collision, const EntityId entity) noexcept
{
    if (!isParticipant(collision, entity))
    {
        return false;
    }

    const bool arrivedFromOutside = collision.previousOverlap.x > 0.0F && collision.previousOverlap.y <= 0.0F;
    const bool nowPenetratingVertically = collision.overlap.y > 0.0F;

    // The mirror of [landedOn]. A ceiling does not have to be static: a moving
    // platform's underside is a ceiling whether it is pushed by code or by gravity, so
    // unlike a floor this one deliberately does not consult the body type. The
    // consequence is bounded - a player under a falling dynamic body is pushed down,
    // which is what a ceiling is - and the alternative would let a player pass upward
    // through anything dynamic, which is worse.
    const bool pushedDown = resolutionFor(collision, entity).y > 0.0F;

    return arrivedFromOutside && nowPenetratingVertically && pushedDown;
}

bool hitSideWith(const Collision& collision, const EntityId entity) noexcept
{
    // The negation of the two vertical cases, rather than a third independent test.
    //
    // Stating it this way is a guarantee: the three outcomes partition every collision
    // a player takes part in, so "a wall hit" can never come to mean something subtly
    // different from "neither a landing nor a ceiling". A test that asserts the
    // partition is what keeps that true as the clauses are edited.
    return isParticipant(collision, entity) && !landedOn(collision, entity) && !hitCeilingWith(collision, entity);
}

Vec2 getOverlap(const Entity& first, const Entity& second) noexcept
{
    const std::optional<std::pair<Aabb, Aabb>> boxes = boxesOf(first, second);
    if (!boxes.has_value())
    {
        return Vec2{0.0F, 0.0F};
    }

    return boxes->first.penetration(boxes->second);
}

Vec2 getPreviousOverlap(const Entity& first, const Entity& second) noexcept
{
    const std::optional<Aabb> firstBox = previousBoxOf(first);
    const std::optional<Aabb> secondBox = previousBoxOf(second);
    if (!firstBox.has_value() || !secondBox.has_value())
    {
        return Vec2{0.0F, 0.0F};
    }

    return firstBox->penetration(*secondBox);
}

} // namespace engine::physics
