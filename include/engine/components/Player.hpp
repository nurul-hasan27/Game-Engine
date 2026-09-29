#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::components
{

/// Which of the course's three player states the player is in.
///
/// The course names them and nothing else: "The player Entity in the game is
/// represented by Megaman, which has several different Animations: Stand, Run, and
/// Air. You must determine which state the player is currently in and assign the
/// correct Animation." Three states, so three values.
///
/// ### Why an enum and not three bools
///
/// `isRunning` and `isAirborne` would allow `isRunning && isAirborne`, which is not
/// a state - and a state machine with an impossible state is a state machine that
/// needs a comment explaining which combination wins. One enum makes every illegal
/// combination unrepresentable rather than merely discouraged.
enum class PlayerState
{
    /// On the ground and not moving horizontally.
    Stand,

    /// On the ground and moving horizontally.
    Run,

    /// Not on the ground. Rising, falling, or in the first frame of a jump.
    Air
};

/// The player's per-frame state, and the point it respawns to.
///
/// ### Why this is a separate component from [PlayerConfig]
///
/// Phase 13 drew that line deliberately and this phase is the reason it was the
/// right line. [PlayerConfig] holds what the **level file** said and never changes:
/// the movement speed, the jump speed, the maximum speed, the gravity, the bullet
/// animation. This holds what changes every frame.
///
/// They are kept apart for the same reason the level's values are not baked into
/// [engine::systems::PlayerSystem]: a system that read its own constants from a
/// component can be handed a different level and behave differently, which is what
/// "the level decides these numbers" has to mean. Keeping the frame state out of
/// `PlayerConfig` is also what lets a test change one without disturbing the other.
///
/// ### Why the respawn anchor is a component and not a system parameter
///
/// The spawn point is a fact about *this* player, read from the level file and
/// converted by the loader. Carrying it on the entity means [engine::systems::PlayerSystem]
/// needs no spawn argument, and it means two players spawned from two different
/// levels in one world would each respawn to their own place - which is a thing the
/// course's multi-level map format makes possible and which a system-wide constant
/// could not express.
struct Player
{
    /// The state the player is in, decided by the system and read by animation
    /// selection. Defaults to [PlayerState::Stand] because a freshly spawned player
    /// is standing still in the air, and its first update corrects it.
    PlayerState state = PlayerState::Stand;

    /// Whether a solid surface is directly under the player's feet.
    ///
    /// Not a "was grounded last frame" flag. It is recomputed every frame by probing
    /// the world, which is what stops it surviving a respawn or a teleport.
    bool grounded = true;

    /// Whether a jump the player started is still in progress.
    ///
    /// Separate from [grounded] because of the first frame of a jump. On that frame
    /// the player is leaving the ground, so a probe still finds the tile it just left
    /// and reports `grounded`. This flag is what makes the jump frame count as
    /// airborne immediately rather than one frame late, and it is what the variable
    /// jump reads to know there is an ascent to cut short.
    ///
    /// Cleared on the frame the player is found grounded again, so it cannot survive
    /// a landing.
    bool jumping = false;

    /// Where this player respawns, in world pixels.
    ///
    /// The same point the loader placed the entity at, so "respawn" restores the
    /// position the level asked for rather than a second, independently computed one.
    Vec2 spawnPosition{0.0F, 0.0F};
};

} // namespace engine::components
