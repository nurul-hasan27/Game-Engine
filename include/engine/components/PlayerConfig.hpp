#pragma once

#include "engine/math/Vec2.hpp"

#include <string>

namespace engine::components
{

/// The player configuration a level file declares, as data.
///
/// ### Why this is a component and not a constructor argument
///
/// The level format puts the player's speed, jump speed, cap, gravity and bullet
/// animation **in the data file**, and the course is explicit that they belong to
/// the level rather than to the code:
///
/// - *"The player has a maximum speed specified in the Level file (see below)
///   which it should not exceed in either x or y direction."*
/// - *"The player will be given a CBoundingBox of a size specified in the level
///   file."*
/// - *"Player GX/Y CW CH SX SY SM GY B"* with `B` the *"Bullet Animation"*.
///
/// So these values change when the level changes, and the entity that owns them
/// changes with them. Putting them on the entity is what lets one player system
/// serve every level: it reads this component and needs to know nothing about how
/// any particular level was authored.
///
/// ### Why the bounding box is not here
///
/// `CW`/`CH` become a [Collider](Collider.hpp), because that is what a collider
/// **is** and because the physics systems already read it. Duplicating the same
/// two numbers into this component as well would be a second copy that could
/// disagree with the first, and there would be no honest way to say which one the
/// physics should believe.
///
/// ### What this deliberately does not contain
///
/// No current velocity, no facing direction, no on-ground flag, no jump state.
/// Every one of those is **per-frame**, and the course is explicit that per-frame
/// state belongs in a system: *"All movement logic should be in the movement
/// system"*, and the action system's *"ONLY"* job is *"to set the proper CInput
/// variables"*. A component holding a velocity would be behaviour's data sitting
/// next to the entity, which is the thing the ECS exists to avoid.
///
/// The values here are **per-level** and never change while the level is loaded,
/// which is exactly the line between the two.
struct PlayerConfig
{
    /// `SX`, left/right movement speed, in pixels per second.
    float leftRightSpeed = 0.0F;

    /// `SY`, the vertical speed a jump starts with, in pixels per second.
    float jumpSpeed = 0.0F;

    /// `SM`, the cap the player must not exceed in either axis, in pixels per
    /// second.
    float maxSpeed = 0.0F;

    /// `GY`, gravity, in pixels per second squared.
    float gravity = 0.0F;

    /// `B`, the animation asset bullets use, by name.
    ///
    /// A name, not a handle and not a resource, for the same reason
    /// [Animation](Animation.hpp) holds a `std::string assetName`: a component
    /// refers to an asset the way the rest of the ECS does, and resolves it through
    /// the [engine::assets::AssetManager] at the point of use. The loader has
    /// already proved this name resolves - it refuses a level whose bullet
    /// animation is missing - so the name here is known good, and still a name
    /// rather than a resource, because the entity must not own one.
    std::string bulletAnimationName;
};

} // namespace engine::components
