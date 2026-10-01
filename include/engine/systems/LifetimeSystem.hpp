#pragma once

#include "engine/ecs/System.hpp"

namespace engine::systems
{

/// Counts [engine::components::Lifetime] down and asks for the entity to be destroyed when
/// it reaches zero.
///
/// ### Why it runs *before* the physics step
///
/// ```text
///   ShootSystem     the Shoot press becomes a bullet entity
///   LifetimeSystem <-- this one: the frame counter expires things that have run out
///   PhysicsSystem   integration and collision - over what is still alive
///   ...
/// ```
///
/// An entity whose last frame is this one should not move this frame, and it certainly
/// should not collide. Running the countdown first makes that true by construction: the
/// entity is flagged before [engine::systems::PhysicsSystem] looks at the world, so it is
/// already out of every query the step makes, and there is no report entry naming it.
///
/// The alternative - counting down after the collision - has a bug that is easy to miss and
/// impossible to see in a screenshot. A bullet on its final frame could register a
/// collision, and [engine::systems::TileSystem] would then have to decide whether an entity
/// that is already dead still gets to blow up the brick it hit. Either answer is
/// defensible, and which one is *right* becomes a question about system ordering that
/// nothing in the game can answer. Removing the question is worth more than the frame.
///
/// ### The other thing this ordering buys
///
/// It is what makes "30 frames" mean exactly 30 frames for a coin.
///
/// [engine::systems::TileSystem] spawns the coin, and it runs *after* this system. So a coin
/// created during frame N is not counted until frame N+1, which means the countdown covers
/// frames N+1 to N+29 and the coin is destroyed during frame N+30: it exists for exactly
/// thirty frames, including the one it was created on, and not thirty-one.
///
/// If the two systems were the other way round, the coin would be decremented on its spawn
/// frame and would last 29. Same number, one frame apart, and the "off by one" would be a
/// fact about the *system order* rather than about the number - which is the worst place for
/// a lifetime to come from.
///
/// ### Nothing structural happens here
///
/// [engine::ecs::EntityManager::destroyEntity] only sets a flag, so requesting destruction
/// during the walk this system is in is explicitly safe. It never creates, never removes a
/// component and never calls [engine::ecs::EntityManager::update], all three of which
/// `Query` documents as invalidating what it is iterating.
class LifetimeSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "LifetimeSystem"; }
};

} // namespace engine::systems