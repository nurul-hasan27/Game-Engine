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
using engine::physics::Collision;

[[nodiscard]] Aabb boxOf(const Transform& transform, const Collider& collider) noexcept
{
    return Aabb{transform.position, collider.size};
}

[[nodiscard]] bool isDynamic(const Body& body) noexcept { return body.type == BodyType::Dynamic; }

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

    // Before anything else, so the report can only ever describe the frame that is
    // running right now.
    //
    // Clearing at the *end* would leave the previous frame readable by anything that
    // runs between this system and the next one's, and clearing lazily would mean a
    // world where physics never ran reported the last thing that happened to it. A
    // stale collision is worse than no collision: a gameplay system that finds a
    // landing in a report describing a frame that has already been resolved would
    // ground a player who is in the air.
    m_collisions.clear();

    // ---- 1. record the previous position, then integrate -------------------
    // Velocity is integrated for every entity that has a Transform, not only
    // those with a Body, so an entity carrying the engine's velocity field keeps
    // behaving the way it did before physics existed. A static body never
    // integrates and has its velocity forced to zero, so nothing can push a
    // wall even if a system mistakenly gives it some.
    for (auto&& [entity, transform] : entities.query<Transform>())
    {
        // Where this body was, *before* this step moved it - ahead of the integration
        // rather than after it.
        //
        // Written for **every** transform, static bodies included, and that is not
        // tidiness. A static body does not move, so its previous position *is* its
        // position - but only if something writes it. Leaving the write below the
        // early-out for static bodies was a real bug: a tile's `prevPosition` stayed at
        // the `{0, 0}` that brace initialisation gave it, so every `previousOverlap`
        // between a player and a tile was measured against a tile sitting at the
        // origin. No player was ever recorded as landing, and the symptom was a player
        // that could not jump.
        transform.prevPosition = transform.position;

        if (const Body* const body = entity.tryGetConstComponent<Body>();
            body != nullptr && !isDynamic(*body))
        {
            transform.velocity = Vec2{0.0F, 0.0F};
            continue;
        }

        //
        // The collision pass below reads this to work out how two bodies arrived at
        // their current overlap, and "arrived" only means anything relative to the
        // position the step started from. Writing it after the integration would make
        // it "where the body was at the end of last frame", which is a different
        // quantity, and every previous overlap in the engine would then be wrong by one
        // frame of movement with nothing failing loudly.
        //
        // This is also the only initialisation `prevPosition` ever needs. A body built
        // by brace initialisation has it at {0, 0}, and this line overwrites that with
        // the real spawn position before anything reads it - so the very first
        // collision in a level already has a correct previous overlap, with no "first
        // frame" special case anywhere in the engine. That is why the field can be the
        // last member of an aggregate that sixty call sites initialise with four
        // values.
        transform.position += transform.velocity * deltaSeconds;
    }

    // ---- 2/3/4/5. detect, resolve, report -----------------------------------
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

            const Aabb firstBox = boxOf(firstTransform, firstCollider);
            const Aabb secondBox = boxOf(secondTransform, secondCollider);

            if (!firstBox.overlaps(secondBox))
            {
                continue;
            }

            // How far the first body must move to leave, and which way. The depth is
            // the penetration, not the shared extent, so a body nested inside another
            // is pushed all the way out instead of part of the way.
            const Vec2 correction = firstBox.minimumTranslation(secondBox);

            const bool firstDynamic = isDynamic(firstBody);
            const bool secondDynamic = isDynamic(secondBody);

            if (!firstDynamic && !secondDynamic)
            {
                // Two immovable bodies. Nothing can be done, and pretending otherwise
                // would make a wall slide around.
                //
                // No record either: this report is "collisions found *and resolved*",
                // and a reader that wants to know whether two arbitrary entities
                // overlap asks `physics::getOverlap` directly, which answers for any
                // pair, two static ones included.
                continue;
            }

            // The evidence, read before the correction destroys it.
            //
            // `overlap` is the penetration that exists right now. `previousOverlap` is
            // the same measurement against `prevPosition` - where these two bodies
            // stood at the top of this step, which is the only moment at which "how did
            // they get here" is still recoverable, because the correction below is
            // precisely what erases it.
            //
            // Neither is read after a position has moved. `correction` is stored
            // oriented towards `first`, and `physics::resolutionFor` reorients it for
            // whoever asks; the sign is what tells a floor from a ceiling.
            Collision record;
            record.first = firstEntity.id();
            record.second = secondEntity.id();
            record.overlap = firstBox.penetration(secondBox);
            record.previousOverlap = Aabb{firstTransform.prevPosition, firstCollider.size}
                                       .penetration(Aabb{secondTransform.prevPosition, secondCollider.size});
            record.firstBody = firstBody.type;
            record.secondBody = secondBody.type;

            // `correction` is stored unchanged in every branch, because the field means
            // "the minimum translation vector, oriented as the push `first` would
            // receive" - a geometric quantity, not a log of what moved. Which body
            // actually moved is a separate fact, and recording the half-push of a
            // dynamic pair here instead would have made the magnitude a lie that
            // `resolutionFor` then had to know about.
            if (firstDynamic && secondDynamic)
            {
                // Split evenly, so the pair is not left overlapping and neither body is
                // favoured. Each body gets half the separation; the record keeps the
                // whole, because the sign is what a consumer needs and the magnitude
                // is the distance to the surface either way.
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
                // `second` takes the opposite push, and the record still holds
                // `correction`: `resolutionFor` negates it for whoever asks about
                // `second`, which is how a player that happened to be visited second
                // still learns it was pushed upward.
                secondTransform.position -= correction;
                resolveVelocity(secondTransform.velocity, -correction);
            }

            record.resolution = correction;
            m_collisions.add(record);
        }
    }
}

} // namespace engine::systems
