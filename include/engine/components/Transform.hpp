#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::components
{

/// An entity's spatial state: where it is, where it is going, how big it is and
/// which way it faces.
///
/// This is the engine's first real component, and it is **pure data**. It has no
/// member functions at all: it does not move, does not render, does not collide,
/// does not know SFML exists, and has never heard of `EntityManager` or `Time`.
/// That is the whole point. Behaviour that reads and writes a Transform belongs
/// to a system, which means it can be added, removed, reordered or replaced
/// without editing a line of this file.
///
/// `Vec2` is `engine::Vec2` from the math foundation, not a second vector type.
///
/// ### Angle
///
/// `angle` is in **radians**, measured counter-clockwise from the positive x
/// axis, matching `Vec2::angle()`. Degrees are never used anywhere in the
/// engine.
///
/// ### Defaults
///
/// | Field | Default | Why |
/// | ----- | ------- | --- |
/// | `position` | `(0, 0)` | the origin |
/// | `velocity` | `(0, 0)` | stationary unless something moves it |
/// | `scale` | `(1, 1)` | unscaled, so rendering needs no special case |
/// | `angle` | `0` | facing along the positive x axis |
///
/// It is an aggregate, so brace initialisation works:
///
/// ```cpp
/// const Transform transform{Vec2{10.0f, 20.0f}, Vec2{2.0f, -1.0f}, Vec2{1.0f, 1.0f}, 0.0f};
/// ```
///
/// Deliberately absent: acceleration, angular velocity, bounds, parent or child
/// links, matrices, and any world/local hierarchy. Each would be speculative
/// until something needs it.
struct Transform
{
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
    float angle = 0.0f;
};

} // namespace engine::components
