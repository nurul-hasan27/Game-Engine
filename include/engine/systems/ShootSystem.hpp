#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/ecs/System.hpp"

namespace engine::systems
{

/// Turns a press of `Shoot` into a bullet.
///
/// ### What it does, and where it sits
///
/// ```text
///   PlayerSystem     movement, jump, gravity, facing, respawn
///   ShootSystem   <-- this one: the Shoot press becomes a bullet entity
///   LifetimeSystem    the frame counter expires things that have run out
///   PhysicsSystem   the bullet is integrated, collides, and is reported
///   ...
/// ```
///
/// Immediately after [PlayerSystem], for two reasons that are both about this frame rather
/// than about elegance:
///
/// - **It reads the player's facing, and [PlayerSystem] is what writes it.** The course's
///   rule is that facing changes when a direction is *pressed*, so the authority for which
///   way a bullet goes is the system that last set `scale.x`. Reading it before that would
///   fire a bullet in the direction the player was facing a frame ago.
/// - **The bullet moves in the frame it is fired.** Before [engine::systems::PhysicsSystem]
///   means the new entity is integrated this frame, is part of this frame's collision pass,
///   and can be in this frame's [engine::physics::CollisionReport]. One frame later would
///   mean a shot whose first frame of motion did not happen.
///
/// ### Edge-triggered, and why that is the course's rule rather than a taste decision
///
/// It asks [engine::input::ActionState::wasPressed], not
/// [engine::input::ActionState::isActive].
///
/// The course states it for the jump key - *"If the jump key is held, the player should not
/// continuously jump, but instead it should only jump once per button press"* - and
/// [engine::components::Action]'s own documentation records the same requirement for
/// shooting: *"The assignment is explicit that holding the key must not produce uncontrolled
/// repeated shots, so gameplay must ask `wasPressed`."*
///
/// With `isActive`, holding Space would create one bullet every frame, and at 60 frames a
/// second that is sixty bullets a second. It would also be a bug the engine could not see:
/// every one of them is a correctly spawned entity that behaves correctly.
///
/// ### What a bullet is made of
///
/// Nothing special. A bullet is an ECS entity with the same components any other moving
/// body has, plus [engine::components::Bullet] to say what it is:
///
/// ```text
///   Transform    where it is, which way it is going
///   Animation    the level's own bullet animation
///   Collider     half the animation's frame size
///   Body         dynamic, so PhysicsSystem integrates it
///   Bullet       "this entity is a projectile"
///   Lifetime     how many frames it has left
/// ```
///
/// There is deliberately no bullet physics. [engine::systems::PhysicsSystem] does not know
/// the word "bullet", and this phase does not add a word for it to know.
///
/// ### Structural change during a walk
///
/// The spawn loop collects its requests and creates the entities **after** the query that
/// found the players has finished. A `Query` borrows the manager's storage, and
/// [engine::ecs::EntityManager::addEntity] invalidates iterators into it even though it
/// does not invalidate references - so spawning inside the walk would be a use-after-
/// invalidate, and would have been for every earlier system too had they needed to spawn.
class ShootSystem final : public engine::ecs::System
{
public:
    /// @param assets The manager the bullet's animation name is resolved through, and
    ///        the one its frame size is read from. Borrowed; the manager outlives the
    ///        system by the same argument every other system in the engine makes.
    explicit ShootSystem(const engine::assets::AssetManager& assets) noexcept : m_assets{&assets} {}

    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "ShootSystem"; }

private:
    const engine::assets::AssetManager* m_assets;
};

} // namespace engine::systems