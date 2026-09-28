#include "engine/systems/PhysicsSystem.hpp"

#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::systems
{
namespace
{

using engine::Vec2;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::ActionState;
using engine::physics::Aabb;
using engine::physics::BodyType;

/// The collision box implied by a transform and a collider.
///
/// Built from `Transform::position`, which Phase 6 defined as the centre, so the
/// box is centred on the entity and needs no anchor adjustment anywhere.
[[nodiscard]] Aabb boxOf(const Transform& transform, const Collider& collider) noexcept
{
    return Aabb{transform.position, collider.size};
}

[[nodiscard]] bool isDynamic(const Body& body) noexcept { return body.type == BodyType::Dynamic; }

/// Zeroes the velocity component that points into the surface the body was just
/// pushed out of.
///
/// `correction` is the offset that was applied to this body, so the surface lies
/// in the direction opposite to it. A velocity component that opposes the
/// correction is heading into that surface and is removed; one that agrees with
/// the correction is already heading away and is left alone, which is what keeps
/// tangential motion intact and stops a body from being frozen by a correction it
/// did not cause.
void resolveVelocity(Vec2& velocity, const Vec2& correction) noexcept
{
    if (correction == Vec2{0.0F, 0.0F})
    {
        return;
    }

    if (correction.x != 0.0F && (velocity.x * correction.x) < 0.0F)
    {
        velocity.x = 0.0F;
    }

    if (correction.y != 0.0F && (velocity.y * correction.y) < 0.0F)
    {
        velocity.y = 0.0F;
    }
}

} // namespace

void PhysicsSystem::update(EntityManager& entities, const ActionState& actions, const float deltaSeconds)
{
    // Physics is a consequence of world velocity, not of this frame's keys.
    static_cast<void>(actions);

    // ---- 1. integrate ----------------------------------------------------
    // Velocity is integrated for every entity that has a Transform, not only
    // those with a Body, so an entity carrying the engine's velocity field keeps
    // behaving the way it did before physics existed. A static body never
    // integrates and has its velocity forced to zero, so nothing can push a
    // wall even if a system mistakenly gives it some.
    for (auto&& [entity, transform] : entities.query<Transform>())
    {
        if (const Body* const body = entity.tryGetConstComponent<Body>();
            body != nullptr && !isDynamic(*body))
        {
            transform.velocity = Vec2{0.0F, 0.0F};
            continue;
        }

        transform.position += transform.velocity * deltaSeconds;
    }

    // ---- 2/3/4. detect, resolve position, resolve velocity ---------------
    // Two independent iterators over the same query, advancing the inner one
    // from just after the outer one. That guarantees i < j, so no entity is
    // compared with itself and every pair is tested exactly once, with no
    // intermediate collection to allocate.
    auto outer = entities.query<Transform, Collider, Body>().begin();
    const auto last = entities.query<Transform, Collider, Body>().end();

    for (; outer != last; ++outer)
    {
        auto inner = outer;
        ++inner;

        for (; inner != last; ++inner)
        {
            auto&& [firstEntity, firstTransform, firstCollider, firstBody] = *outer;
            auto&& [secondEntity, secondTransform, secondCollider, secondBody] = *inner;
            static_cast<void>(firstEntity);
            static_cast<void>(secondEntity);

            const Aabb firstBox = boxOf(firstTransform, firstCollider);
            const Aabb secondBox = boxOf(secondTransform, secondCollider);

            if (!firstBox.overlaps(secondBox))
            {
                continue;
            }

            // How far the first body must move to leave, and which way.
            const Vec2 correction = firstBox.minimumTranslation(secondBox);

            const bool firstDynamic = isDynamic(firstBody);
            const bool secondDynamic = isDynamic(secondBody);

            if (!firstDynamic && !secondDynamic)
            {
                // Two immovable bodies. Nothing can be done, and pretending
                // otherwise would make a wall slide around.
                continue;
            }

            if (firstDynamic && secondDynamic)
            {
                // Split evenly, so the pair is not left overlapping and neither
                // body is favoured.
                firstTransform.position += correction * 0.5F;
                secondTransform.position -= correction * 0.5F;

                resolveVelocity(firstTransform.velocity, correction);
                resolveVelocity(secondTransform.velocity, -correction);
            }
            else if (firstDynamic)
            {
                firstTransform.position += correction;
                resolveVelocity(firstTransform.velocity, correction);
            }
            else
            {
                secondTransform.position -= correction;
                resolveVelocity(secondTransform.velocity, -correction);
            }
        }
    }
}

} // namespace engine::systems
