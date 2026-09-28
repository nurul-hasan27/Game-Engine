#include "engine/graphics/SfmlRenderer.hpp"

// The definition of assets::Texture::Impl, so the pixels behind a handle can be
// read. SfmlRenderer is one of exactly two friends of Texture for this reason: it
// is the engine's other SFML boundary, and drawing requires the platform
// resource. Nothing else in the engine can reach inside a handle.
#include "AssetHandleNative.hpp"

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/RectangleShape.hpp>
#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Text.hpp>
#include <SFML/Graphics/Transform.hpp>
#include <SFML/System/Vector2.hpp>

#include <algorithm>
#include <stdexcept>

namespace engine::graphics
{
namespace
{

/// Clamps an engine colour channel in [0, 1] onto the 0-255 byte SFML wants.
/// Clamping rather than truncating means an out-of-range channel produces a
/// sensible colour instead of wrapping around.
[[nodiscard]] sf::Uint8 toByte(const float channel) noexcept
{
    return static_cast<sf::Uint8>(std::clamp(channel, 0.0F, 1.0F) * 255.0F);
}

[[nodiscard]] sf::Color toSfmlColor(const Color& color) noexcept
{
    return sf::Color{toByte(color.red), toByte(color.green), toByte(color.blue), toByte(color.alpha)};
}

/// Turns a placement into render states, shared by every draw call.
///
/// Factored out rather than written twice because the two draws *must* agree: a
/// rectangle and a texture that were composed separately would eventually drift,
/// and a sprite that scaled about a different point from a rectangle would be a
/// miserable bug to find. One function makes the agreement structural.
///
/// translate, then rotate, then scale. Composed this way the object scales and
/// rotates about its own centre and then moves to its position, which is the
/// order a reader expects, and is unchanged from the rectangle-only version of
/// this file.
[[nodiscard]] sf::RenderStates statesFor(const RenderTransform& placement)
{
    sf::Transform transform;
    transform.translate(placement.position.x, placement.position.y);
    transform.rotate(placement.rotationDegrees);
    transform.scale(placement.scale.x, placement.scale.y);

    sf::RenderStates states;
    states.transform = transform;
    return states;
}

} // namespace

SfmlRenderer::SfmlRenderer(sf::RenderWindow& window) noexcept : m_window{&window}, m_frameCount{0}
{
}

void SfmlRenderer::beginFrame()
{
    // Nothing to do: SFML clears and presents implicitly. The method exists
    // because the frame protocol is part of the Renderer contract, and an
    // implementation may well need per-frame state.
}

void SfmlRenderer::clear(const Color& color)
{
    m_window->clear(toSfmlColor(color));
}

void SfmlRenderer::drawRectangle(const Vec2& size, const Color& color, const RenderTransform& placement)
{
    sf::RectangleShape shape{sf::Vector2f{size.x, size.y}};
    shape.setFillColor(toSfmlColor(color));

    // The engine says position is the centre, SFML's rectangle starts at its
    // top-left, so the origin moves to the middle here rather than leaking an
    // anchor field into the component.
    shape.setOrigin(sf::Vector2f{size.x * 0.5F, size.y * 0.5F});

    m_window->draw(shape, statesFor(placement));
}

void SfmlRenderer::drawTexture(const assets::Texture& texture, const RenderTransform& placement,
                               const std::optional<IntRect>& source)
{
    // An empty handle has no pixels. Drawing nothing would be a silent failure
    // that shows up much later as a missing sprite with no explanation, so this
    // reports it at the point of the mistake instead.
    if (texture.m_impl == nullptr)
    {
        throw std::logic_error{"SfmlRenderer::drawTexture called with an empty texture handle"};
    }

    const sf::Texture& native = texture.m_impl->native;
    const sf::Vector2u size = native.getSize();

    // The whole image unless a region was named. An empty region draws nothing,
    // which is a legitimate thing for a caller to ask for - an animation asked for
    // a frame it does not have should produce nothing rather than the whole
    // texture, and it is not worth an exception.
    const IntRect region = source.value_or(IntRect{0, 0, static_cast<int>(size.x), static_cast<int>(size.y)});

    if (isEmpty(region))
    {
        return;
    }

    sf::Sprite sprite{native};

    // The origin moves to the middle of the region being drawn, not the middle of
    // the image. This is the whole difference between a sprite and an animation of
    // it: a sprite sheet is one image holding many frames, and each frame has to be
    // centred on the entity rather than the sheet, or an animated character would
    // shift sideways every time it changed frame.
    //
    // For a whole-image draw the region is the image, so this is the same origin
    // the previous single-rectangle path computed. Nothing that used to be drawn
    // moves by a pixel.
    sprite.setOrigin(sf::Vector2f{static_cast<float>(region.width) * 0.5F,
                                  static_cast<float>(region.height) * 0.5F});

    // The one place `engine::IntRect` becomes `sf::IntRect`. Above this line the
    // engine has no graphics types at all.
    sprite.setTextureRect(sf::IntRect{region.left, region.top, region.width, region.height});

    // Same states function as the rectangle, so a sprite and a rectangle at the
    // same placement land in the same place at the same size.
    m_window->draw(sprite, statesFor(placement));
}

void SfmlRenderer::drawText(const assets::Font& font, const std::string& content, const std::uint32_t characterSize,
                           const Color& color, const RenderTransform& placement)
{
    // Same rule as `drawTexture`, and for the same reason: an empty handle has no
    // glyphs, and drawing nothing would be a silent failure that surfaces much later
    // as missing text with nothing pointing at the cause.
    if (font.m_impl == nullptr)
    {
        throw std::logic_error{"SfmlRenderer::drawText called with an empty font handle"};
    }

    // An empty string measures 0x0 (measured, not assumed - see the note on the
    // origin below), so there is nothing to place and nothing to draw. Returning
    // early keeps that explicit rather than letting a zero-size object through the
    // transform.
    if (content.empty())
    {
        return;
    }

    sf::Text text;
    text.setFont(font.m_impl->native);
    text.setString(content);
    text.setCharacterSize(characterSize);
    text.setFillColor(toSfmlColor(color));

    // ### Why the origin is measured, and why it is not `width / 2`
    //
    // The engine's rule is that a position is the **centre** of what is drawn, so
    // the origin has to be the middle of the string. A string's extent is not
    // `characterSize * n`, though - it depends on the font's metrics, on the
    // characters chosen and on the size asked for, and it is only knowable by
    // asking.
    //
    // The part that is easy to get wrong is the *left edge*. `getLocalBounds()`
    // returns a rectangle whose `left` is not always zero: measured against the
    // three committed fonts, `tech` reports a left of -1, -2 and -4 at character
    // sizes 12, 24 and 48, because its glyphs have a left side bearing. So the
    // centre is `left + width / 2` and **not** `width / 2` - the latter would put
    // `tech` text up to four pixels right of where every other renderable in the
    // engine would put it, for a reason invisible in a screenshot and obvious in a
    // bounds measurement.
    //
    // The top edge is worse still: `numbers` reports a top of +3 at size 12 while
    // `pixeled` reports -2, so the baseline sits at a different height per font.
    // Again the only honest source is the measured bounds.
    const sf::FloatRect bounds = text.getLocalBounds();
    text.setOrigin(bounds.left + (bounds.width * 0.5F), bounds.top + (bounds.height * 0.5F));

    // The same `statesFor` the rectangle and the sprite use, so a string moves,
    // scales and rotates under the same transform everything else does. Text is
    // world space in this engine - the course reference draws its grid overlay at
    // world pixel coordinates - so the camera has already been applied by the
    // caller through `toRenderTransform`, and nothing here re-applies it.
    m_window->draw(text, statesFor(placement));
}

void SfmlRenderer::endFrame()
{
    m_window->display();
    ++m_frameCount;
}

} // namespace engine::graphics
