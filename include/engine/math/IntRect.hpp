#pragma once

namespace engine
{

/// An axis-aligned rectangle in whole pixels, addressed from its top-left corner.
///
/// ### Why this type exists
///
/// The course's animation mechanism is a *texture rectangle*: a run of frames
/// lives inside one image and the renderer is told which part of it to show. That
/// is a region **inside** a texture, so it is naturally whole pixels, and it is
/// naturally anchored top-left, because that is the only corner a pixel grid has
/// an unambiguous meaning for.
///
/// The engine already has its own `Vec2` and its own `Color` for exactly the
/// reason it needs its own `IntRect`: so that components, assets and the
/// `Renderer` interface can talk about appearance without a graphics library type
/// appearing in any of them. `sf::IntRect` is converted to and from inside
/// `SfmlRenderer.cpp` and is never named above that file.
///
/// ### Deliberately not an Aabb
///
/// [engine::physics::Aabb](Aabb.hpp) is the *other* rectangle in this engine and
/// it is built from a centre and half extents, because it describes where an
/// object **is**. This one is built from a top-left corner and a size, because it
/// describes which part of an image is being sampled. Mixing the two conventions
/// in one type would mean every call site had to know which question it was
/// asking, and getting it wrong shifts an image by half its size.
///
/// ### Empty is representable, and is not a whole texture
///
/// A default-constructed rect is all zeroes: an empty region. That is a real,
/// drawable-nothing state and it is what a caller gets by forgetting to set
/// something. It is deliberately **not** overloaded to mean "the whole image" -
/// see `std::optional` at the draw call for how the whole-image case is
/// expressed instead.
struct IntRect
{
    /// Distance from the left edge of the sampled image, in pixels.
    int left = 0;

    /// Distance from the top edge of the sampled image, in pixels.
    int top = 0;

    /// Width in pixels. Zero is an empty region.
    int width = 0;

    /// Height in pixels. Zero is an empty region.
    int height = 0;
};

/// A rect covering an entire image, as the source rectangle for a whole-texture
/// draw.
///
/// It is a named constant rather than a value a caller constructs, so the
/// "whole image" case is written the same way everywhere it appears.
inline constexpr IntRect kWholeImage{0, 0, 0, 0};

/// True when the region contains no pixels.
///
/// A zero width or a zero height means nothing can be sampled. Negative extents
/// are also empty: an inverted rectangle cannot be walked left to right, and
/// treating it as empty is more predictable than letting it through to a graphics
/// library that may or may not complain.
[[nodiscard]] constexpr bool isEmpty(const IntRect& rect) noexcept
{
    return rect.width <= 0 || rect.height <= 0;
}

/// Exact comparison, matching [engine::Color](Color.hpp). C++17 does not give
/// aggregates an implicit `operator==`, so it is written out here. Exact rather
/// than approximate on purpose: a tolerance hidden inside a value type hides a
/// real decision.
[[nodiscard]] constexpr bool operator==(const IntRect& lhs, const IntRect& rhs) noexcept
{
    return lhs.left == rhs.left && lhs.top == rhs.top && lhs.width == rhs.width && lhs.height == rhs.height;
}

[[nodiscard]] constexpr bool operator!=(const IntRect& lhs, const IntRect& rhs) noexcept { return !(lhs == rhs); }

} // namespace engine
