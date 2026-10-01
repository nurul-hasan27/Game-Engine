#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/ecs/System.hpp"
#include "engine/physics/Collision.hpp"

namespace engine::systems
{

/// Acts on the tiles a collision involved: bricks explode, question blocks are used, coins
/// appear and bullets stop.
///
/// ### It reads Phase 17's report, and runs immediately after the systems that fill it
///
/// ```text
///   PhysicsSystem        integrates, resolves, and records what overlapped and how
///   PlayerStateSystem  <-- reads the report too: grounded, Stand/Run/Air
///   TileSystem        <-- this one: what the collisions *mean* for level geometry
///   CameraSystem        follows where the player ended up
///   AnimationSystem     advances the explosion this system started
/// ```
///
/// Every fact this system needs is already computed:
/// [engine::physics::Collision] carries both participants, the current overlap, the
/// **previous** overlap, the push that was applied and both body types. Nothing here runs
/// an AABB test of its own, and there is no second collision implementation to disagree
/// with the first. A bullet cannot "detect" a tile by looking for one nearby; it asks what
/// the physics step recorded.
///
/// Immediately after [engine::systems::PhysicsSystem], obviously - before it there is no
/// report. Before [engine::systems::AnimationSystem], because the explosion this system
/// starts is advanced the same frame it starts, which is what makes an explosion appear on
/// the frame the brick breaks rather than a frame later.
///
/// ### Why the record is read once and not kept
///
/// The report is a frame's observation and nothing survives it:
/// [engine::systems::PhysicsSystem] clears it at the top of every update. So this system
/// copies the two entity **ids** out of it into local vectors and works from those, and it
/// holds no state between frames. Keeping a `Collision` - or a reference to one - across a
/// frame would be a dangling record describing a resolution that has already been applied.
///
/// Ids are safe to keep for as long as the system needs them because
/// [engine::ecs::EntityManager] never reuses one.
///
/// ### What it does not do
///
/// - It does not resolve anything. The player has already been pushed out of the brick by
///   the time this system runs; what it does next is change what the brick *is*, not where
///   the player is.
/// - It does not advance an animation. The explosion is
///   [engine::components::Animation]'s business and
///   [engine::systems::AnimationSystem]'s, and the entity is removed when the animation
///   reports itself ended.
/// - It does not scan for tiles, and it does not keep a list of them. Everything it acts on
///   was found by an [engine::ecs::Query] on the same frame, so a tile created or destroyed
///   elsewhere this frame is simply the one it finds.
///
/// ### Walking the world four times, and never while changing it
///
/// [engine::ecs::Query] documents that `addEntity`, `update` and `removeComponent` on a
/// *matching* entity are unsupported mid-iteration. So this system separates the walks:
///
/// 1. every bullet's collisions, copied out as id pairs - and the bullet destroyed here,
///    which is safe because destruction is only a flag;
/// 2. the player's from-below collisions, likewise;
/// 3. every tile, which is where the animations and colliders change - and neither
///    [engine::components::Animation] nor [engine::components::Collider] is removed from a
///    *matching* entity, so nothing this walk does can invalidate it;
/// 4. after every walk has finished, the coins are created.
///
/// Step 3 removes [engine::components::Collider] from a brick that is matching the walk's
/// **own** query. That is safe for one specific reason worth stating: `Collider` is not one
/// of the queried types, so removing it cannot dangle the reference the walk is holding and
/// cannot change whether the entity matches.
class TileSystem final : public engine::ecs::System
{
public:
    /// @param collisions The frame's report, produced by the
    ///        [engine::systems::PhysicsSystem] whose `collisions()` was passed here.
    ///        Borrowed for the same reason [engine::systems::PlayerStateSystem]'s is: it is
    ///        a member of another system, and two worlds in one process must not see each
    ///        other's collisions.
    /// @param assets The manager the question block's and the coin's animation names are
    ///        resolved through, and the one the coin's frame size is read from.
    TileSystem(const engine::physics::CollisionReport& collisions, const engine::assets::AssetManager& assets) noexcept
        : m_collisions{&collisions},
          m_assets{&assets}
    {
    }

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions,
                float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "TileSystem"; }

private:
    /// Creates the coin a question block drops, `kCoinSpawnHeight` above `position`.
    ///
    /// Called only after every walk has finished, because it creates an entity.
    void spawnCoin(engine::ecs::EntityManager& entities, const engine::Vec2 position) const;

    const engine::physics::CollisionReport* m_collisions;
    const engine::assets::AssetManager* m_assets;
};

} // namespace engine::systems