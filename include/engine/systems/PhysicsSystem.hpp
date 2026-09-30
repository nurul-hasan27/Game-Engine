#pragma once

#include "engine/ecs/System.hpp"
#include "engine/physics/Aabb.hpp"
#include "engine/physics/Collision.hpp"

namespace engine
{
namespace input
{
class Input;
}
} // namespace engine

namespace engine::systems
{

/// Integrates velocity, then detects and resolves collisions.
///
/// ### The step, in order
///
/// 1. **Record, then integrate.** Every body's
///    [engine::components::Transform::prevPosition] is set to its current position
///    *before* it moves, and then it moves by `velocity * deltaSeconds`. The order is
///    the contract: `prevPosition` means "where this body was before the step that is
///    running now", and writing it after the integration would make it "where it was
///    at the end of last frame" - a different quantity, and one that would make every
///    previous overlap wrong by a frame of movement without anything failing.
/// 2. **Detect.** Every pair of collidable entities is tested, once each.
/// 3. **Resolve position.** Overlapping pairs are pushed apart along the axis
///    of least penetration, split between two dynamic bodies and taken in full
///    by a dynamic one facing a static one.
/// 4. **Resolve velocity.** Any velocity component directed *into* the surface
///    a body was just pushed out of is zeroed. Velocity along the other axis
///    survives, which is what lets a body slide along a wall.
/// 5. **Report.** Every pair that overlapped is recorded in [collisions], carrying
///    its current overlap, its previous overlap, and the push applied. All three are
///    read from the positions as they were *before* the correction, because the
///    correction is the thing that destroys that evidence.
///
/// ### Ordering
///
/// This must be registered **after** `MovementSystem` and before rendering.
/// `MovementSystem` sets velocity from input; this system consumes it. Register
/// it first and every body would be integrated with the previous frame's
/// velocity, lagging input by a frame.
///
/// The input parameter is unused. Physics is a consequence of the world's
/// velocity, not of what the player is pressing this frame.
///
/// ### Deliberately simple
///
/// Discrete detection, an intentionally naive O(N^2) broad phase, and
/// positional correction with no impulses, friction or bounce. See
/// [docs/physics.md](../../docs/physics.md) for the full list of limitations,
/// including tunnelling, which this phase does not attempt to solve.
class PhysicsSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "PhysicsSystem"; }

    /// Every collision found and resolved by the most recent [update].
    ///
    /// ### This is the phase's whole point, and it is owned rather than global
    ///
    /// Before this phase the physics system detected, resolved and then forgot: the
    /// only trace of a collision was that a position had moved. Gameplay had to
    /// re-derive "did I land?" by probing the world for something to stand on, which
    /// is a second, disagreeing implementation of a question physics had already
    /// answered - and the reason landing detection was a frame late.
    ///
    /// So the answer is kept, for one frame, in a form a gameplay system can read:
    /// [engine::physics::Collision] carries the two participants, the current overlap,
    /// the **previous** overlap, and the push that was applied. The previous overlap is
    /// what makes a landing distinguishable from a wall, and it is only knowable during
    /// the step - once the positions are corrected the evidence is gone.
    ///
    /// A member rather than a singleton, so a world with two physics systems - which a
    /// test may well build - has two independent reports, and a gameplay system reads
    /// exactly the one it was handed. See [engine::physics::CollisionReport] for the
    /// lifetime rules.
    [[nodiscard]] const engine::physics::CollisionReport& collisions() const noexcept { return m_collisions; }

private:
    /// Cleared at the top of every [update], so it can never describe a frame that has
    /// already been resolved.
    engine::physics::CollisionReport m_collisions;
};

} // namespace engine::systems
