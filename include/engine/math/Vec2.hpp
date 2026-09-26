#pragma once

namespace engine
{

/// A 2D vector, used for points, directions, velocities, sizes and scales.
///
/// Coordinates are `float`, matching SFML's own 2D types, and every angle in
/// this class is in radians. Vec2 is a small value type with no dependencies:
/// it deliberately includes no SFML header, so the engine's math layer stays
/// independent of the graphics library and can be reasoned about and tested on
/// its own.
///
/// ### Conventions
///
/// - Angles are radians, measured from the positive x axis and increasing
///   counter-clockwise, as returned by atan2(y, x).
/// - `normalized()` of the zero vector returns the zero vector. The zero vector
///   has no direction, so there is nothing meaningful to return, and producing
///   NaN or infinity would poison everything downstream.
/// - Dividing by a zero scalar returns the zero vector, and dividing in place by
///   zero resets the vector to zero. This keeps the type from producing
///   infinity and NaN; see operator/ and operator/=.
/// - `operator==` and `operator!=` compare exactly. There is deliberately no
///   epsilon-based comparison here; code that needs a tolerance should state it
///   at the call site rather than hide it inside the type.
///
/// Everything that is plain arithmetic is `constexpr` and lives in this header.
/// The operations that need the C math library (sqrt, atan2, sin, cos) are
/// declared here and defined in Vec2.cpp.
class Vec2
{
public:
    /// Creates the zero vector (0, 0).
    constexpr Vec2() noexcept = default;

    /// Creates a vector with the given components.
    constexpr Vec2(const float xValue, const float yValue) noexcept : x{xValue}, y{yValue} {}

    float x = 0.0f;
    float y = 0.0f;

    // -- Unary ----------------------------------------------------------------

    /// Returns the vector pointing the opposite way.
    [[nodiscard]] constexpr Vec2 operator-() const noexcept { return Vec2{-x, -y}; }

    // -- Compound assignment --------------------------------------------------

    constexpr Vec2& operator+=(const Vec2& other) noexcept
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    constexpr Vec2& operator-=(const Vec2& other) noexcept
    {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    constexpr Vec2& operator*=(const float scalar) noexcept
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    /// Divides in place by `scalar`. A zero scalar resets the vector to zero.
    constexpr Vec2& operator/=(const float scalar) noexcept
    {
        if (scalar == 0.0f)
        {
            x = 0.0f;
            y = 0.0f;
            return *this;
        }

        x /= scalar;
        y /= scalar;
        return *this;
    }

    // -- Comparison -----------------------------------------------------------

    /// Exact comparison. See the class documentation on why there is no
    /// epsilon-based alternative here.
    [[nodiscard]] constexpr bool operator==(const Vec2& other) const noexcept
    {
        return x == other.x && y == other.y;
    }

    [[nodiscard]] constexpr bool operator!=(const Vec2& other) const noexcept
    {
        return !(*this == other);
    }

    // -- Measurements ---------------------------------------------------------

    /// Squared length, i.e. x * x + y * y. Cheaper than length() and enough for
    /// comparisons, since length() is monotonic in the squared length.
    [[nodiscard]] constexpr float lengthSquared() const noexcept { return x * x + y * y; }

    /// Length of the vector, i.e. sqrt(x * x + y * y).
    [[nodiscard]] float length() const noexcept;

    /// Dot product, i.e. x * other.x + y * other.y.
    [[nodiscard]] constexpr float dot(const Vec2& other) const noexcept
    {
        return x * other.x + y * other.y;
    }

    /// Euclidean distance from this point to `other`.
    [[nodiscard]] float distance(const Vec2& other) const noexcept;

    // -- Direction ------------------------------------------------------------

    /// A unit-length copy of this vector.
    ///
    /// Returns the zero vector when this vector is zero, or too small for its
    /// squared length to be representable, so the result is never NaN.
    [[nodiscard]] Vec2 normalized() const noexcept;

    /// Direction of this vector in radians, in the range (-pi, pi], measured
    /// counter-clockwise from the positive x axis.
    ///
    /// Returns 0 for the zero vector, which has no direction.
    [[nodiscard]] float angle() const noexcept;

    /// A copy of this vector rotated around the origin by `radians`
    /// counter-clockwise. The zero vector is returned unchanged.
    [[nodiscard]] Vec2 rotated(const float radians) const noexcept;
};

// -- Binary operators -------------------------------------------------------
//
// The symmetric operators are free functions rather than members, so argument
// order never matters: that is what lets both `v * 2` and `2 * v` work.
// Compound assignment stays a member, since it has an obvious left operand.

[[nodiscard]] constexpr Vec2 operator+(const Vec2& lhs, const Vec2& rhs) noexcept
{
    return Vec2{lhs.x + rhs.x, lhs.y + rhs.y};
}

[[nodiscard]] constexpr Vec2 operator-(const Vec2& lhs, const Vec2& rhs) noexcept
{
    return Vec2{lhs.x - rhs.x, lhs.y - rhs.y};
}

[[nodiscard]] constexpr Vec2 operator*(const Vec2& vector, const float scalar) noexcept
{
    return Vec2{vector.x * scalar, vector.y * scalar};
}

[[nodiscard]] constexpr Vec2 operator*(const float scalar, const Vec2& vector) noexcept
{
    return vector * scalar;
}

/// Divides by `scalar`. A zero scalar yields the zero vector.
[[nodiscard]] constexpr Vec2 operator/(const Vec2& vector, const float scalar) noexcept
{
    if (scalar == 0.0f)
    {
        return Vec2{};
    }

    return Vec2{vector.x / scalar, vector.y / scalar};
}

} // namespace engine
