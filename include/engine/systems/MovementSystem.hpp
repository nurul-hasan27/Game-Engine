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
/// ### This is a reference system, not a movement feature
///
/// It exists to demonstrate the whole chain end to end: Input, query, Transform,
/// delta time, RenderSystem. It is deliberately the simplest thing that can be
/// called movement.
///
/// In particular:
///
/// - **No acceleration.** A held key moves at full speed on the very first
///   frame. There is no ramp-up, inertia or friction.
/// - **Velocity is not written.** `Transform::velocity` is left exactly as it
///   was, because the position is derived directly from the input direction.
///   Repurposing velocity as "last frame's direction" would quietly redefine a
///   field other systems may read, and that belongs to a real movement model if
///   one is ever wanted.
/// - **No physics.** No gravity, no bounds, no collision, no ground.
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
