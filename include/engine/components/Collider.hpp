#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::components
{

/// The shape used for collision detection.
///
/// Deliberately separate from `Rectangle`, which is the shape used for drawing.
/// A game routinely wants them to differ: a player sprite is much larger than
/// the player's collider, a wide grass tile has a small collider, and a
/// projectile's visual can trail behind its hitbox. Keeping them apart means
/// neither has to know about the other, and neither contains the other.
///
/// Pure data, and in the same centre-based space as `Transform::position`: the
/// box is centred on the transform's position. A negative size is treated as
/// zero, and a zero size is a degenerate point rather than an error.
struct Collider
{
    /// Width and height of the box, in pixels. Centred on
    /// `Transform::position`.
    Vec2 size{0.0F, 0.0F};
};

} // namespace engine::components
