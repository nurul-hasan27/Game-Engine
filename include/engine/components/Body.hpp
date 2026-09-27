#pragma once

#include "engine/physics/Aabb.hpp"

namespace engine::components
{

/// How the simulation treats an entity's motion.
///
/// This is physical state, not a shape: an entity has a `Body` saying whether
/// it moves at all, and a separate `Collider` saying what it looks like to the
/// collision test. An entity can have a `Body` and no `Collider` (it moves but
/// nothing can bump into it) or a `Collider` and no `Body` (a static obstacle,
/// which is the usual case for a wall).
struct Body
{
    /// Dynamic or Static. Default Dynamic, so a body added with no arguments
    /// behaves like the moving thing most bodies are.
    physics::BodyType type = physics::BodyType::Dynamic;
};

} // namespace engine::components
