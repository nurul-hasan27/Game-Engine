#pragma once

#include "engine/ecs/System.hpp"

namespace engine
{
namespace input
{
class Input;
}
} // namespace engine

namespace engine::systems
{

/// Moves every entity with a `components::Transform` according to the keys the
/// player is currently holding.
///
/// | Key | Direction |
/// | --- | --------- |
/// | `W` or `Up` | up |
/// | `S` or `Down` | down |
/// | `A` or `Left` | left |
/// | `D` or `Right` | right |
///
/// ### This system sets velocity; physics moves the body
///
/// As of Phase 8 this system **assigns** `Transform::velocity` and leaves
/// `Transform::position` alone. `PhysicsSystem` integrates that velocity. Before
/// Phase 8 the system added `direction * speed * deltaSeconds` straight to the
/// position, which cannot coexist with a physics system: two systems writing
/// position would each move the body, and the results would compound.
///
/// The observable behaviour is unchanged. Setting `velocity = direction * speed`
/// and then integrating `position += velocity * deltaSeconds` lands the body in
/// exactly the same place as the old one-liner did, because integration is
/// linear. `tests/PhysicsTest.cpp` asserts that equivalence directly, and the
/// Phase 7 movement tests still hold.
///
/// Velocity is **assigned** rather than accumulated, and is zero when no key is
/// held, so releasing the keys stops the body instead of leaving it gliding.
/// That also means something else cannot contribute to velocity without this
/// system overwriting it; composing several sources of motion is a later
/// concern.
///
/// ### This is a reference system, not a movement feature
///
/// It exists to demonstrate the whole chain end to end: Input, query, Transform,
/// delta time, physics, RenderSystem. It is deliberately the simplest thing that
/// can be called movement.
///
/// In particular:
///
/// - **No acceleration.** A held key produces full speed immediately. There is
///   no ramp-up, inertia or friction.
/// - **No physics of its own.** It has never applied gravity, and it does not
///   collide; `PhysicsSystem` does both.
/// - **It does not know which entity is the player.** It sets velocity on every
///   entity with a `Transform`, and `PhysicsSystem` is what stops a static wall
///   from moving in response.
///
/// There is deliberately no `Player` class. A player is an entity holding a
/// `Transform` and a `Rectangle`, and being moved is something a system does to
/// it. Adding a `Player` type would move behaviour back next to the data, which
/// is the thing the ECS exists to avoid.
class MovementSystem final : public engine::ecs::System
{
public:
    /// Speed in pixels per second. Defaults to 100, and is deliberately not
    /// derived from a frame rate: a faster machine must not move faster.
    static constexpr float kDefaultSpeed = 100.0F;

    explicit MovementSystem(const float speed = kDefaultSpeed) noexcept : m_speed{speed} {}

    void update(engine::ecs::EntityManager& entities, input::Input& input, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "MovementSystem"; }

    /// Pixels per second.
    [[nodiscard]] float speed() const noexcept { return m_speed; }

private:
    float m_speed;
};

} // namespace engine::systems
