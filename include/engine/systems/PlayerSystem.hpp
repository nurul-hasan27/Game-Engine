#pragma once

#include "engine/ecs/System.hpp"

namespace engine::systems
{

/// The player's behaviour: movement, jumping, facing, the state machine, animation
/// selection and respawn.
///
/// ### What this system owns, and what it does not
///
/// It owns **intent and the player's own state**: what the player is trying to do
/// this frame, which of the course's three states that puts them in, which way they
/// face, and whether they have fallen out of the world.
///
/// It does **not** own motion. It writes [engine::components::Transform]'s velocity
/// and gravity pulls on it; [engine::systems::PhysicsSystem] integrates that velocity
/// and resolves the collision. There is one integrator in this engine and this
/// system is not it - which is the difference between a player behaviour system and a
/// second physics engine wearing a hat.
///
/// It does not own frames either. [engine::systems::AnimationSystem] advances
/// [engine::components::Animation]; this system only chooses *which* animation the
/// player is in. No frame counting happens here.
///
/// ### Where it goes in the update order
///
/// ```text
/// PlayerSystem      this one: intent, jump, gravity, facing, state, respawn
/// PhysicsSystem     velocity -> position, then collision resolution
/// CameraSystem      the player's world position -> the camera
/// ZoomKeysSystem    zoom actions -> the camera's zoom
/// AnimationSystem   the chosen animation's frame index
/// ```
///
/// It must be **before** [engine::systems::PhysicsSystem], because the velocity it
/// writes is the velocity physics integrates this same frame. A player that moved
/// one frame late would feel it: pressing right would visibly delay the start of
/// the walk by a frame, and the delay would be different on the first frame after a
/// transition than on the hundredth.
///
/// It is **after** nothing, because nothing before it produces input other than
/// [engine::Application] building the action snapshot, and it is given that snapshot
/// already resolved.
///
/// It is **before** [engine::systems::AnimationSystem] so the animation chosen this
/// frame is the one advanced this frame. Reversed, a state change would be visible
/// one frame late, which is a stutter in the walk cycle at every state boundary.
///
/// [engine::systems::CameraSystem] is after physics for a reason that predates this
/// system and still holds: the camera should follow where the player *ended* up this
/// frame, not where they started it.
///
/// ### Grounded is a probe, and that is a Phase 17 dependency
///
/// Nothing in this engine reports "a collision happened" to the system that caused
/// it, so this system cannot ask whether it is standing on something. Instead it
/// probes: is there a static collider whose top surface is level with the player's
/// feet and overlapping them horizontally?
///
/// That is the smallest thing that supports the state machine, and it is exactly
/// enough. It is **not** the collision refinement Phase 17 will own, and it has one
/// visible consequence: on the frame the player lands, the probe is looking at the
/// position the *previous* frame's collision left behind, so gravity is applied for
/// one frame after touchdown before physics zeroes the velocity. The player does not
/// sink and does not bounce - the correction is exact, only the *detection* is a frame
/// late. Phase 17 is where landing becomes reportable rather than inferred.
///
/// ### The maximum speed is applied to the horizontal axis only
///
/// The course's Assignment 3 says the player "has a maximum speed specified in the
/// Level file (see below) which it should not exceed in either x or y direction", and
/// its own reference leaves a `TODO` reading "implement the maximum player speed in
/// both X and Y directions". This system clamps the **x** axis only.
///
/// The reason is the committed level's own numbers. `assets/levels/level1.txt` gives
/// `SY 400` (jump speed) and `SM 250` (maximum speed). Clamping y as well would
/// truncate every jump to 250 on the frame it launched, which makes the level's jump
/// speed unreachable and reduces the jump's peak from
/// `400^2 / (2 * 900) = 88.9` pixels to `250^2 / (2 * 900) = 34.7` - a rise smaller
/// than the player's own 60-pixel collider, so the player could not clear a single
/// 64-pixel tile. A speed limit that silently deletes the level's jump speed and
/// makes the vertical movement unplayable is a worse reading than the one it
/// replaces.
///
/// The consequence is recorded rather than hidden: the player's peak upward speed is
/// the level's `jumpSpeed`, which is above the level's `maxSpeed` for the first
/// fraction of a second of every jump. Clamping y is a single line in
/// [PlayerSystem::update], and it is deliberately one line rather than a refactor, so
/// that reversing this decision later is a change and not an argument.
class PlayerSystem final : public engine::ecs::System
{
public:
    /// @param fallLimitY The world coordinate below which the player has fallen out
    ///        of the level, in world pixels.
    ///
    ///        A constructor argument and not a constant, because it is a property of
    ///        the level rather than of the player. [engine::scene::PlayScene] passes
    ///        the world height it built the grid with, so a level of a different size
    ///        respawns at a different height without this file changing. It is
    ///        measured, not invented: it is the bottom edge of the world the level
    ///        was loaded into.
    explicit PlayerSystem(const float fallLimitY) noexcept : m_fallLimitY{fallLimitY} {}

    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "PlayerSystem"; }

    /// The fall limit this system was built with, for tests and diagnostics.
    [[nodiscard]] float fallLimitY() const noexcept { return m_fallLimitY; }

private:
    float m_fallLimitY;
};

} // namespace engine::systems
