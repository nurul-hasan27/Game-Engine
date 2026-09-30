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

    /// Where this body was immediately before the physics step that is running now.
    ///
    /// ### Why it is the **last** member, and that is load bearing
    ///
    /// This component is aggregate-initialised in roughly sixty places with four
    /// values, and it must stay trivially copyable - `RenderTest` asserts that with a
    /// `static_assert`. So it cannot gain a constructor that defaults this field to
    /// `position`, the way the course's `CTransform` does it.
    ///
    /// Placing it first would therefore be a silent disaster: every one of those
    /// four-value initialisations would assign its `velocity` to `prevPosition`. It
    /// goes last so that they leave it at `{0, 0}` and nothing else changes.
    ///
    /// ### Why `{0, 0}` is nevertheless correct
    ///
    /// Because nothing reads this field before the physics step writes it.
    /// [engine::systems::PhysicsSystem] does, in this order:
    ///
    /// ```text
    ///   pass 1   prevPosition = position;            <-- always written first
    ///            position    += velocity * deltaSeconds;
    ///   pass 2   the collision pass reads prevPosition
    /// ```
    ///
    /// So on the very first frame the uninitialised value is overwritten with the
    /// real spawn position before the collision pass can look at it, and from then on
    /// it is by construction "the position before *this* physics update".
    ///
    /// That is also why the write has to stay in pass 1 and ahead of the integration.
    /// Moving it after the integration would make it "the position after last frame",
    /// which is a different quantity, and every previous-frame overlap in the engine
    /// would be wrong by one frame of movement without anything failing loudly.
    Vec2 prevPosition{0.0f, 0.0f};
};

} // namespace engine::components
