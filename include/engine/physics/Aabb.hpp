#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::physics
{

/// A body classification.
///
/// Only the two kinds this phase needs. `Kinematic` is deliberately absent: it
/// would need a "pushed by a system but not by velocity and not displaced by
/// collision" rule, and nothing in this phase wants one.
enum class BodyType
{
    /// Moved by integrating its velocity, and displaceable by collision.
    Dynamic,

    /// Never moves. Acts as an immovable obstacle that dynamic bodies collide
    /// with. Its velocity is forced to zero every step, so nothing can push it.
    Static
};

namespace detail
{

/// Component-wise clamp to zero, so a negative size degrades to zero rather than
/// producing an inverted, min-above-max box.
[[nodiscard]] constexpr Vec2 nonNegative(const Vec2& value) noexcept
{
    return Vec2{value.x < 0.0F ? 0.0F : value.x, value.y < 0.0F ? 0.0F : value.y};
}

} // namespace detail

/// An axis-aligned bounding box, stored the same way the rest of the engine
/// reasons about space: **centre plus half extents**.
///
/// Phase 6 established that `Transform.position` is the centre of the drawn
/// object, and this keeps that single convention everywhere. An AABB is never
/// built from a top-left corner anywhere in the engine, so nothing has to
/// convert between the two forms.
///
/// `min()` and `max()` are derived rather than stored, so the representation
/// cannot drift out of sync with itself.
///
/// ### Touching is not overlapping
///
/// `overlaps()` is strict. Two boxes that share only an edge, with no shared
/// area, do **not** overlap. That is what lets a body rest exactly on a
/// surface without being reported as penetrating it every frame.
class Aabb
{
public:
    /// Builds a box from its centre and its full size.
    ///
    /// A negative size is treated as zero, so a box is never inverted.
    ///
    /// A zero-size box is a degenerate point, and because `overlaps()` is
    /// strict, a point has zero extent and therefore **never** overlaps
    /// anything. A collider with a zero size is inert rather than an error,
    /// which is the simplest predictable rule.
    constexpr Aabb(const Vec2& center, const Vec2& size) noexcept
        : m_center{center}, m_halfExtents{detail::nonNegative(size) * 0.5F}
    {
    }

    [[nodiscard]] constexpr Vec2 center() const noexcept { return m_center; }
    [[nodiscard]] constexpr Vec2 halfExtents() const noexcept { return m_halfExtents; }
    [[nodiscard]] constexpr Vec2 size() const noexcept { return m_halfExtents * 2.0F; }

    [[nodiscard]] constexpr Vec2 min() const noexcept { return m_center - m_halfExtents; }
    [[nodiscard]] constexpr Vec2 max() const noexcept { return m_center + m_halfExtents; }

    /// Signed overlap on the x axis.
    ///
    /// Positive means the projections share width, so the boxes are
    /// interpenetrating horizontally. Zero means they touch exactly. Negative
    /// means they are separated horizontally.
    [[nodiscard]] constexpr float overlapX(const Aabb& other) const noexcept
    {
        const float lowest = this->min().x > other.min().x ? this->min().x : other.min().x;
        const float highest = this->max().x < other.max().x ? this->max().x : other.max().x;
        return highest - lowest;
    }

    /// Signed overlap on the y axis, with the same sign convention as
    /// overlapX().
    [[nodiscard]] constexpr float overlapY(const Aabb& other) const noexcept
    {
        const float lowest = this->min().y > other.min().y ? this->min().y : other.min().y;
        const float highest = this->max().y < other.max().y ? this->max().y : other.max().y;
        return highest - lowest;
    }

    /// True when the boxes share area on **both** axes.
    ///
    /// Strict: touching edges are not an overlap, so a body resting on a
    /// surface is not in penetration.
    [[nodiscard]] constexpr bool overlaps(const Aabb& other) const noexcept
    {
        return overlapX(other) > 0.0F && overlapY(other) > 0.0F;
    }

    /// Which axis the boxes interpenetrate on most shallowly, and by how much.
    ///
    /// Returns `None` when the boxes do not overlap. This is the choice that
    /// makes resolution push a body out along the shortest route.
    enum class PenetrationAxis
    {
        None,
        Horizontal,
        Vertical
    };

    /// The axis of least penetration, and the depth along it.
    ///
    /// Resolution happens on the axis with the **smaller** overlap, so a body
    /// is pushed out by the shortest distance rather than being squeezed
    /// through a nearby face.
    ///
    /// **Tie behaviour:** an exact tie resolves on the vertical axis. The choice
    /// is arbitrary, but it is fixed, which is what determinism requires.
    [[nodiscard]] constexpr PenetrationAxis penetrationAxis(const Aabb& other) const noexcept
    {
        if (!overlaps(other))
        {
            return PenetrationAxis::None;
        }

        return overlapX(other) < overlapY(other) ? PenetrationAxis::Horizontal : PenetrationAxis::Vertical;
    }

    /// The minimum translation vector: the offset that would push **this** box
    /// out of `other`, along the axis of least penetration.
    ///
    /// Only meaningful when `overlaps(other)` is true; on touching or
    /// separated boxes it yields a zero or meaningless vector, so callers must
    /// check first. `PhysicsSystem` always does.
    ///
    /// The sign comes from the relative centres, never from a fixed direction:
    /// whichever box is further left is pushed left. That makes the result
    /// correct for both orientations of the same pair.
    [[nodiscard]] constexpr Vec2 minimumTranslation(const Aabb& other) const noexcept
    {
        const float horizontal = overlapX(other);
        const float vertical = overlapY(other);

        if (horizontal < vertical)
        {
            return Vec2{m_center.x < other.m_center.x ? -horizontal : horizontal, 0.0F};
        }

        return Vec2{0.0F, m_center.y < other.m_center.y ? -vertical : vertical};
    }

private:
    Vec2 m_center{0.0F, 0.0F};
    Vec2 m_halfExtents{0.0F, 0.0F};
};

} // namespace engine::physics
