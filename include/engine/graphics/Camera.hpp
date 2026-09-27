#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::graphics
{

/// The smallest zoom the camera will hold.
///
/// A zoom of zero or less has no meaningful inverse, so rather than let
/// `screenToWorld()` divide by zero and produce nonsense, the value is clamped
/// here, in the setter. That keeps the invariant "zoom is always positive" true
/// for the whole engine: nothing downstream has to re-check it.
///
/// Clamping is preferred over rejecting because a camera that refuses to render
/// because someone assigned it a bad number is worse than one that renders at an
/// extreme magnification. The engine's existing rule is the same, in `Vec2`:
/// `normalized()` returns the zero vector rather than NaN, and division by a zero
/// scalar returns the zero vector rather than infinity. Refuse to produce
/// unrepresentable values, never crash and never silently misbehave.
///
/// 1e-4 is small enough that a caller cannot notice the clamp, and large enough
/// that `screenToWorld()` stays comfortably finite.
inline constexpr float kMinimumZoom = 1e-4F;

/// A 2D view onto the world.
///
/// ### World space and screen space
///
/// The world is wherever gameplay says it is. Entities live in world
/// coordinates, physics runs in world coordinates, and input means world
/// directions. The screen is the rectangle of pixels actually presented. Until
/// this phase those were the same space; now a `Camera` is the one thing that
/// converts between them.
///
/// ### The one rule
///
/// **Entities are never moved to follow the camera.** A `Transform.position` is
/// a world position and always will be. The camera transforms a position only
/// when it is being drawn, and nothing else in the engine ever sees a screen
/// position. That is what lets physics, movement and input stay expressed in
/// world coordinates with no knowledge that a camera exists.
///
/// ### The mapping
///
/// The camera is **centre based**: `position` is the world point shown at the
/// middle of the viewport. For a viewport of `width` by `height`, the screen
/// centre is `(width / 2, height / 2)`, and
///
/// ```text
/// screenPosition = (worldPosition - cameraPosition) * zoom + screenCenter
/// ```
///
/// At zoom 1 and a camera at the origin, a world point maps to itself plus the
/// screen centre, so the top-left of the world appears at the top-left of the
/// screen.
///
/// `screenToWorld()` is the exact inverse:
///
/// ```text
/// worldPosition = (screenPosition - screenCenter) / zoom + cameraPosition
/// ```
///
/// ### Zoom
///
/// Zoom is a rendering concern and nothing else. It multiplies the apparent size
/// of everything, and it deliberately does **not** touch `Transform.scale`,
/// `Collider::size`, velocity or any other simulation data. A body that is 50
/// units wide is still 50 units wide and still collides the same way at any zoom.
///
/// ### Deliberately absent
///
/// No rotation, no smoothing, no damping, no spring, no shake, no bounds
/// clamping, no frustum culling, and no second vector type. See
/// [docs/camera.md](../../docs/camera.md) for the full list.
///
/// This type is SFML-free by construction: it includes only `engine::Vec2`. The
/// engine does not use `sf::View`, because `sf::View` would make the world to
/// screen contract a property of the graphics library rather than of the engine,
/// and would leak into every system that wanted to know what is on screen.
class Camera
{
public:
    /// The world point shown at the middle of the viewport.
    [[nodiscard]] constexpr Vec2 position() const noexcept { return m_position; }

    /// Apparent scale. Always greater than zero, because `setZoom` clamps.
    [[nodiscard]] constexpr float zoom() const noexcept { return m_zoom; }

    /// The size of the visible area, in pixels. Defaults to zero, which is a
    /// legitimate "no viewport configured yet" state rather than an error: the
    /// screen centre then sits at the origin and world coordinates map straight
    /// through. Nothing divides by the viewport, so no value of it can produce
    /// an invalid result.
    [[nodiscard]] constexpr Vec2 viewport() const noexcept { return m_viewport; }

    /// Moves the view. Ordinary world coordinates, no unit, no conversion.
    constexpr void setPosition(const Vec2& position) noexcept { m_position = position; }

    /// Sets the apparent scale. Zero and negative values are clamped to
    /// `kMinimumZoom`; see that constant for why clamping beats rejecting.
    constexpr void setZoom(const float zoom) noexcept
    {
        m_zoom = zoom > kMinimumZoom ? zoom : kMinimumZoom;
    }

    /// Sets the size of the visible area, in pixels.
    constexpr void setViewport(const Vec2& viewport) noexcept { m_viewport = viewport; }

    /// The middle of the viewport, in screen pixels. This is where the camera's
    /// own world position is drawn.
    [[nodiscard]] constexpr Vec2 screenCenter() const noexcept { return m_viewport * 0.5F; }

    /// World position to screen position.
    [[nodiscard]] constexpr Vec2 worldToScreen(const Vec2& worldPosition) const noexcept
    {
        return (worldPosition - m_position) * m_zoom + screenCenter();
    }

    /// Screen position to world position. The exact inverse of `worldToScreen()`,
    /// and total for every input, because `m_zoom` is always positive.
    [[nodiscard]] constexpr Vec2 screenToWorld(const Vec2& screenPosition) const noexcept
    {
        return (screenPosition - screenCenter()) / m_zoom + m_position;
    }

private:
    Vec2 m_position{0.0F, 0.0F};
    Vec2 m_viewport{0.0F, 0.0F};
    float m_zoom{1.0F};
};

} // namespace engine::graphics
