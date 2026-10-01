#pragma once

#include <string_view>

namespace engine::components
{

/// The entity tag given to a bullet the player has fired.
///
/// Bullets are identified by [Bullet](Bullet.hpp) rather than by this tag - the tag is
/// for humans reading a debugger, and for tests that want to ask "how many bullets are
/// there?" without knowing what else is in the world. Nothing in the engine branches on
/// it.
inline constexpr std::string_view kBulletTag = "combat.bullet";

/// Marks an entity as one of the player's bullets, and nothing else.
///
/// ### Why it has no members
///
/// Because there is nothing about a bullet that is not already somewhere else, and every
/// field added here would be a second copy of a fact that could then disagree:
///
/// - **Where it is going** is [engine::components::Transform]'s `velocity`, written once
///   at spawn from the player's facing and never touched again. A bullet does not steer,
///   does not accelerate and does not respond to anything, so its velocity *is* its
///   direction, and reading it is reading the truth rather than a duplicate.
/// - **How it is drawn** is an [engine::components::Animation] naming the level's bullet
///   animation, exactly as every other sprite in the engine.
/// - **What it collides with** is a [engine::components::Collider] and
///   [engine::components::Body], which is how *every* collidable entity works. A bullet is
///   not a special case in [engine::systems::PhysicsSystem], and this is why it need not be.
/// - **How long it lives** is [engine::components::Lifetime].
///
/// What none of those say is "this particular entity is a projectile", and that is the
/// one fact gameplay needs. So the component's entire content is its own existence: a
/// bullet is an entity that carries `components::Bullet`.
///
/// ### What this replaces, and why neither of those is acceptable
///
/// The course's reference identifies a bullet by an entity **id** (`addEntity(DYNAMIC,
/// BULLET)` stores the tile type in the entity's `id()` field) and by comparing animation
/// names. Neither is a fact about the entity rather than about the process that made it:
///
/// - An id is assigned in creation order, so "id < 32 is a tile" is a statement about
///   what happened to be spawned first. A level that loaded one extra entity would
///   reclassify the rest.
/// - An animation *name* is a rendering choice. A bullet drawn with different artwork
///   would stop being a bullet, and a brick drawn with the same artwork would become one.
///
/// A semantic component is the engine's existing answer to "which entity is this", the
/// same one [engine::components::Player] already uses for the player, and it cannot go
/// stale.
struct Bullet
{
};

} // namespace engine::components