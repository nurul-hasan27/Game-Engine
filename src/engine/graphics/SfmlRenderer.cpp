#include "engine/graphics/SfmlRenderer.hpp"

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/RectangleShape.hpp>
#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Transform.hpp>
#include <SFML/System/Vector2.hpp>

#include <algorithm>

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

    // translate, then rotate, then scale. Composed this way the object scales
    // and rotates about its own centre and then moves to its position, which is
    // the order a reader expects.
    sf::Transform transform;
    transform.translate(placement.position.x, placement.position.y);
    transform.rotate(placement.rotationDegrees);
    transform.scale(placement.scale.x, placement.scale.y);

    sf::RenderStates states;
    states.transform = transform;

    m_window->draw(shape, states);
}

void SfmlRenderer::endFrame()
{
    m_window->display();
    ++m_frameCount;
}

} // namespace engine::graphics
