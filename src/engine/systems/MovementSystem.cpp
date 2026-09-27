#include "engine/systems/MovementSystem.hpp"

#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::systems
{
namespace
{

/// Builds the unit direction the player is asking for.
///
/// Which keys mean which direction is gameplay meaning, so it lives here in the
/// system rather than in Input. Input only knows which physical keys are held.
[[nodiscard]] Vec2 directionFrom(const input::Input& input) noexcept
{
    Vec2 direction{0.0F, 0.0F};

    // y decreases upwards, which matches the screen's y-down axis: "up" is -y.
    if (input.isKeyDown(input::Key::W) || input.isKeyDown(input::Key::Up))
    {
        direction.y -= 1.0F;
    }

    if (input.isKeyDown(input::Key::S) || input.isKeyDown(input::Key::Down))
    {
        direction.y += 1.0F;
    }

    if (input.isKeyDown(input::Key::A) || input.isKeyDown(input::Key::Left))
    {
        direction.x -= 1.0F;
    }

    if (input.isKeyDown(input::Key::D) || input.isKeyDown(input::Key::Right))
    {
        direction.x += 1.0F;
    }

    // Normalising is what stops W+D from moving sqrt(2) times as fast as W
    // alone. Vec2::normalized returns the zero vector for the zero vector, so
    // "no keys held" needs no special case and produces no movement.
    return direction.normalized();
}

} // namespace

void MovementSystem::update(engine::ecs::EntityManager& entities, input::Input& input,
                            const float deltaSeconds)
{
    // Physics owns integration, so this system's job ends at setting a
    // velocity. deltaSeconds is accepted for the uniform signature and is
    // deliberately not used: velocity is per second, and applying it to
    // position here as well would move the body twice.
    static_cast<void>(deltaSeconds);

    const Vec2 direction = directionFrom(input);
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
