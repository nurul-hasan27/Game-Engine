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

void SfmlRenderer::drawTexture(const assets::Texture& texture, const RenderTransform& placement)
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

    // The whole image, centred, the same convention as a rectangle. No source
    // rectangle: frames are a later concern and there is nothing to select one
    // from yet.
    sf::Sprite sprite{native};
    sprite.setOrigin(sf::Vector2f{static_cast<float>(size.x) * 0.5F, static_cast<float>(size.y) * 0.5F});

    // Same states function as the rectangle, so a sprite and a rectangle at the
    // same placement land in the same place at the same size.
    m_window->draw(sprite, statesFor(placement));
}

void SfmlRenderer::endFrame()
{
    m_window->display();
    ++m_frameCount;
}

} // namespace engine::graphics
