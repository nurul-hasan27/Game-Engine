#pragma once

#include "engine/graphics/Renderer.hpp"

#include <cstdint>

// Forward declared on purpose: this header stays includable without a single
// SFML header. The concrete graphics types live entirely in the .cpp, so the
// size of the SFML surface exposed to the rest of the engine is exactly one
// class.
namespace sf
{
class RenderWindow;
} // namespace sf

namespace engine::graphics
{

/// The SFML implementation of Renderer.
///
/// This is the engine's one intentional SFML boundary for rendering. It
/// **references** a window it does not own: the window belongs to
/// `Application`, which outlives the renderer and owns the graphics context.
/// Referencing rather than owning keeps window lifetime in exactly one place,
/// and keeps RAII intact: the window's destructor still closes it.
///
/// Angle handling: engine radians become SFML degrees in
/// `toRenderTransform`, and the sign is not flipped, because engine world space
/// and screen space share the same axes here. See docs/rendering.md.
class SfmlRenderer final : public Renderer
{
public:
    /// Binds to a window this renderer does not own. The window must outlive
    /// the renderer.
    explicit SfmlRenderer(sf::RenderWindow& window) noexcept;

    void beginFrame() override;
    void clear(const Color& color) override;
    void drawRectangle(const Vec2& size, const Color& color, const RenderTransform& placement) override;
    void endFrame() override;

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_frameCount; }

private:
    sf::RenderWindow* m_window = nullptr;
    std::uint64_t m_frameCount = 0;
};

} // namespace engine::graphics
