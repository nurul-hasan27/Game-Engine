#pragma once

#include "engine/ecs/System.hpp"
#include "engine/physics/Collision.hpp"

namespace engine::systems
{

/// Reconciles the player's state from the collisions the physics step just resolved.
///
/// ### Why this is a separate system, and not part of [PlayerSystem]
///
/// Because the answer it needs does not exist yet when [PlayerSystem] runs.
/// [PlayerSystem] has to write the velocity that [engine::systems::PhysicsSystem]
/// integrates *in the same frame* - that ordering is why the walk starts on the frame
/// the key goes down rather than a frame later - and the collision that says "the
/// player landed" is a consequence of that integration. So the state has to be read
/// after, not before.
///
/// Registering it separately is the honest way to say that. The alternative, having
/// [PlayerSystem] read a report that physics has not written yet, would reintroduce
/// exactly the one-frame lag this phase exists to remove.
///
/// ### It does no physics
///
/// This system reads a report and writes two components. It does not integrate, does
/// not detect, does not resolve, and does not search the world for a floor. There is
/// still exactly one integration and one collision pass per frame, in
/// [engine::systems::PhysicsSystem] - which is the constraint that keeps this from
/// becoming a second physics step wearing a hat.
///
/// ### The order it has to be in
///
/// ```text
///   PlayerSystem       writes velocity: movement, jump, gravity, respawn
///   PhysicsSystem      integrates it, resolves it, and reports what overlapped
///   PlayerStateSystem  <-- this one: reads the report, writes grounded/state/picture
///   CameraSystem       follows where the player ended up
///   ZoomKeysSystem     zoom
///   AnimationSystem    advances the frame of the picture chosen above
/// ```
///
/// After [engine::systems::PhysicsSystem], or [player.grounded] would be a frame
/// stale. Before [engine::systems::AnimationSystem], or the picture would be chosen
/// after the frame index advanced - a stutter at every state boundary.
///
/// ### What "grounded" now means, precisely
///
/// **A support collision was resolved this frame.** Not "there is something under the
/// player", not "the player was under something last frame". One definition, computed
/// from the frame's own report, with no tolerance and no second opinion.
///
/// The two consequences worth naming:
///
/// - Leaving the ground takes effect on the frame it happens. Walk off a ledge and
///   there is no support collision, so the player is airborne immediately - Phase 16
///   needed an extra frame for its probe to notice the absence of a tile.
/// - Landing takes effect on the frame it happens, which is the Phase 16 limitation
///   this phase was written to remove.
class PlayerStateSystem final : public engine::ecs::System
{
public:
    /// @param collisions The report to read, owned by the
    ///        [engine::systems::PhysicsSystem] that produced it.
    ///
    ///        A reference rather than a pointer so it cannot be null, and not owned so
    ///        two worlds in one process cannot see each other's collisions. The
    ///        reference stays valid because
    ///        [engine::ecs::SystemManager] holds systems through `unique_ptr`
    ///        specifically so that registering another one does not move the ones
    ///        already there - so handing out a reference from `add` is safe, and
    ///        [engine::scene::PlayScene] does exactly that.
    explicit PlayerStateSystem(const engine::physics::CollisionReport& collisions) noexcept
        : m_collisions{&collisions}
    {
    }

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions,
                float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "PlayerStateSystem"; }

private:
    const engine::physics::CollisionReport* m_collisions;
};

} // namespace engine::systems
