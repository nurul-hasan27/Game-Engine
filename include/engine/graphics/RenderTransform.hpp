#pragma once

#include "engine/components/Transform.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::graphics
{

/// A placement in render space: what the renderer actually needs, with units
/// already converted and nothing left to interpret.
///
/// This exists so that the interesting part of rendering, turning engine state
/// into render state, is a pure function. It has no SFML types, allocates
/// nothing and can be unit tested without a window, which is why the conversion
/// does not live inside the renderer.
///
/// See [docs/rendering.md](docs/rendering.md) for the full coordinate contract.
struct RenderTransform
{
    /// Centre of the object, in pixels from the top-left of the window.
    Vec2 position{0.0F, 0.0F};

    /// Per-axis scale multiplier. `(1, 1)` is unscaled.
    Vec2 scale{1.0F, 1.0F};

    /// Clockwise rotation in degrees, matching SFML's convention.
    float rotationDegrees = 0.0F;
};

/// Degrees per radian, as a plain constant so tests can refer to it.
inline constexpr float kDegreesPerRadian = 57.295779513082320876798154814105F;

/// Converts a components::Transform into render-space placement.
///
/// The only change is the angle unit. Engine angles are radians; SFML wants
/// degrees. The sign is deliberately **not** negated: engine world space and
/// screen space share the same axes in this phase (x right, y down), and both
/// apply the same rotation matrix, so a direct unit conversion already produces
/// the correct visual result. Negating here would introduce a double flip.
[[nodiscard]] constexpr RenderTransform toRenderTransform(const components::Transform& transform) noexcept
{
    return RenderTransform{transform.position, transform.scale, transform.angle * kDegreesPerRadian};
}

} // namespace engine::graphics
