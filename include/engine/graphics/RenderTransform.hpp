#pragma once

#include "engine/components/Transform.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::graphics
{

/// A placement in screen space: what the renderer actually needs, with units
/// already converted and the camera already applied, and nothing left to
/// interpret.
///
/// This exists so that the interesting part of rendering, turning engine state
/// into render state, is a pure function. It has no SFML types, allocates
/// nothing and can be unit tested without a window, which is why the conversion
/// does not live inside the renderer.
///
/// See [docs/rendering.md](docs/rendering.md) for the full coordinate contract
/// and [docs/camera.md](docs/camera.md) for the world to screen mapping.
struct RenderTransform
{
    /// Centre of the object, in screen pixels from the top-left of the window.
    ///
    /// This is a **screen** position, not a world one. It is the only place in
    /// the whole engine that a world to screen conversion happens, and it
    /// happens here rather than inside the renderer so the renderer stays a
    /// service that puts rectangles where it is told.
    Vec2 position{0.0F, 0.0F};

    /// Per-axis scale multiplier. `(1, 1)` is unscaled.
    ///
    /// This carries the camera's zoom, so the visual size of an object is
    /// `size * scale * zoom`. Zoom lives here, in the object's own scale, rather
    /// than being folded into `position`, and that placement matters: `position`
    /// is absolute screen space while `scale` is local to the object's centre.
    /// Folding the zoom into `position` as well would scale the translation and
    /// drag objects away from where they belong.
    Vec2 scale{1.0F, 1.0F};

    /// Clockwise rotation in degrees, matching SFML's convention.
    ///
    /// Unchanged by the camera, which does not rotate the world. A zoomed-in
    /// rotated object simply renders larger.
    float rotationDegrees = 0.0F;
};

/// Degrees per radian, as a plain constant so tests can refer to it.
inline constexpr float kDegreesPerRadian = 57.295779513082320876798154814105F;

/// Converts a world-space components::Transform into screen-space placement.
///
/// This is the whole of the world to screen conversion, and it is a pure
/// function of the transform and the camera. Three things happen, and only
/// three:
///
/// 1. **Position** goes through `Camera::worldToScreen()`. This is the only
///    place a world position becomes a screen position.
/// 2. **Scale** is multiplied by the camera's zoom, which is the only place zoom
///    affects anything. Simulation data is untouched.
/// 3. **Angle** is converted from radians to degrees. The sign is deliberately
///    **not** negated: world space and screen space share the same axes (x right,
///    y down), and both apply the same rotation matrix, so a direct unit
///    conversion already produces the correct visual result. Negating here would
///    introduce a double flip.
///
/// The camera is a **required** parameter rather than an optional one on purpose.
/// An overload that silently ignored it would let a caller render a world
/// position straight to the screen and get a subtly wrong picture instead of a
/// compile error, which is the worst available outcome.
///
/// The camera is a parameter rather than a singleton so this stays a pure
/// function that any test can call with its own camera.
[[nodiscard]] constexpr RenderTransform toRenderTransform(const components::Transform& transform,
                                                          const Camera& camera) noexcept
{
    return RenderTransform{camera.worldToScreen(transform.position), transform.scale * camera.zoom(),
                           transform.angle * kDegreesPerRadian};
}

/// A placement whose position is already in screen pixels, with the camera not applied.
///
/// ### The counterpart to [toRenderTransform]
///
/// [toRenderTransform] is what makes a world position land in the right pixel: it
/// moves the point by the camera and multiplies the size by the zoom. This does
/// neither, so an entity placed with it keeps its position and its size no matter
/// where the camera is or how far it is zoomed.
///
/// That is exactly what a menu wants, and it is why the two are separate functions
/// rather than a flag on one. A flag would have to be threaded through every call
/// site, and the two answers are different enough - one ignores the camera, one
/// requires it - that a shared signature would hide which is wanted.
///
/// ### What it still applies
///
/// Scale and rotation, unchanged. They belong to the entity rather than to the
/// camera, so a screen-space label can still be scaled or turned; what it cannot do
/// is be *moved* or *magnified* by the camera, which is the whole point.
///
/// ### The angle conversion is repeated on purpose
///
/// The degrees conversion is one multiplication, and a helper called from two
/// places to perform it would be a place for the two to disagree. A rotated
/// screen-space entity has to be rotated by the same rule as a rotated world one,
/// and the test that checks they agree checks both of these.
[[nodiscard]] constexpr RenderTransform toScreenTransform(const components::Transform& transform) noexcept
{
    return RenderTransform{transform.position, transform.scale, transform.angle * kDegreesPerRadian};
}

} // namespace engine::graphics
