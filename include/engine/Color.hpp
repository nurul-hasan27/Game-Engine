#pragma once

namespace engine
{

/// A straight (non-premultiplied) RGBA colour with channels in [0, 1].
///
/// The engine has its own colour for the same reason it has its own `Vec2`: so
/// that components and systems can talk about appearance without any SFML type
/// leaking into the ECS. The Renderer is the only place that converts to
/// `sf::Color`.
///
/// Channels are expected in [0, 1] and are clamped when converted, so a
/// component that computes a colour slightly out of range produces a sane
/// result instead of an undefined one.
struct Color
{
    float red = 1.0F;
    float green = 1.0F;
    float blue = 1.0F;
    float alpha = 1.0F;
};

/// Opaque black, the natural "no colour" default.
inline constexpr Color kBlack{0.0F, 0.0F, 0.0F, 1.0F};

/// Opaque white.
inline constexpr Color kWhite{1.0F, 1.0F, 1.0F, 1.0F};

/// Exact comparison, matching Vec2. C++17 does not give aggregates an implicit
/// operator==, so it is written out here. Exact rather than approximate on
/// purpose: a tolerance hidden inside a value type hides a real decision.
[[nodiscard]] constexpr bool operator==(const Color& lhs, const Color& rhs) noexcept
{
    return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue && lhs.alpha == rhs.alpha;
}

[[nodiscard]] constexpr bool operator!=(const Color& lhs, const Color& rhs) noexcept { return !(lhs == rhs); }

} // namespace engine
