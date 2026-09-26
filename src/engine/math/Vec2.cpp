#include "engine/math/Vec2.hpp"

#include <cmath>

namespace engine
{

float Vec2::length() const noexcept
{
    return std::sqrt(lengthSquared());
}

float Vec2::distance(const Vec2& other) const noexcept
{
    return (*this - other).length();
}

Vec2 Vec2::normalized() const noexcept
{
    const float squaredLength = lengthSquared();

    if (squaredLength == 0.0f)
    {
        // No direction to preserve. operator/ would also catch a zero divisor
        // and return zero, so this check is not strictly required today. It is
        // kept because normalized() promises never to produce NaN, and that
        // guarantee should hold here on its own terms rather than depend on
        // operator/'s behaviour. It also covers squared length underflowing to
        // zero for a vector whose components are too small to square.
        return Vec2{};
    }

    return *this / std::sqrt(squaredLength);
}

float Vec2::angle() const noexcept
{
    if (lengthSquared() == 0.0f)
    {
        // The zero vector has no direction. atan2(0, 0) is specified to return
        // 0, but relying on that would make the contract less obvious.
        return 0.0f;
    }

    return std::atan2(y, x);
}

Vec2 Vec2::rotated(const float radians) const noexcept
{
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);

    return Vec2{x * cosine - y * sine, x * sine + y * cosine};
}

} // namespace engine
