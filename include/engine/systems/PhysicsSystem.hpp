#pragma once

#include "engine/ecs/System.hpp"
#include "engine/physics/Aabb.hpp"

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
/// 1. **Integrate.** Every dynamic body moves by `velocity * deltaSeconds`.
/// 2. **Detect.** Every pair of collidable entities is tested, once each.
/// 3. **Resolve position.** Overlapping pairs are pushed apart along the axis
///    of least penetration, split between two dynamic bodies and taken in full
///    by a dynamic one facing a static one.
/// 4. **Resolve velocity.** Any velocity component directed *into* the surface
///    a body was just pushed out of is zeroed. Velocity along the other axis
///    survives, which is what lets a body slide along a wall.
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
    void update(engine::ecs::EntityManager& entities, input::Input& input, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "PhysicsSystem"; }
};

} // namespace engine::systems
