#include "engine/systems/MovementSystem.hpp"

#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::systems
{
namespace
{

/// Builds the unit direction the player is asking for.
///
/// Asks about actions and never about keys. Which physical key means "move left"
/// is [input::ActionMap]'s business; what left *means* - a unit step towards the
/// left edge of the screen - is gameplay, and belongs here.
///
/// The four direction actions are asked for independently rather than being
/// derived from one another, so the arrows in the default map need no special
/// case and a rebinding cannot make the system inconsistent with itself.
[[nodiscard]] Vec2 directionFrom(const input::ActionState& actions) noexcept
{
    Vec2 direction{0.0F, 0.0F};

    // y decreases upwards, which matches the screen's y-down axis: "up" is -y.
    if (actions.isActive(input::Action::MoveUp))
    {
        direction.y -= 1.0F;
    }

    if (actions.isActive(input::Action::MoveDown))
    {
        direction.y += 1.0F;
    }

    if (actions.isActive(input::Action::MoveLeft))
    {
        direction.x -= 1.0F;
    }

    if (actions.isActive(input::Action::MoveRight))
    {
        direction.x += 1.0F;
    }

    // Normalising is what stops MoveUp+MoveRight from moving sqrt(2) times as far
    // as MoveUp alone. Vec2::normalized returns the zero vector for the zero
    // vector, so "nothing held" needs no special case and produces no movement.
    return direction.normalized();
}

} // namespace

void MovementSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                            const float deltaSeconds)
{
    // Physics owns integration, so this system's job ends at setting a
    // velocity. deltaSeconds is accepted for the uniform signature and is
    // deliberately not used: velocity is per second, and applying it to
    // position here as well would move the body twice.
    static_cast<void>(deltaSeconds);

    const Vec2 direction = directionFrom(actions);
    const Vec2 velocity = direction * m_speed;

    for (auto&& [entity, transform] : entities.query<components::Transform>())
    {
        static_cast<void>(entity);
        // Assigned, not accumulated, and zero when nothing is held, so releasing
        // the keys stops the body instead of leaving it gliding.
        transform.velocity = velocity;
    }
}

} // namespace engine::systems
