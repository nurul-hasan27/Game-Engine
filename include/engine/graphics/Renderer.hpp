#pragma once

#include "engine/Color.hpp"
#include "engine/assets/Texture.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/math/Vec2.hpp"

#include <cstdint>

namespace engine::graphics
{

/// The engine's view of "put this on screen".
///
/// This is the entire graphics vocabulary the rest of the engine knows, and it
/// contains no SFML type. `RenderSystem` depends on this interface, never on an
/// implementation, which is what lets the system be unit tested against a
/// recording fake with no window and no GPU.
///
/// The frame protocol is begin, clear, draw any number of times, end:
///
/// ```cpp
/// renderer.beginFrame();
/// renderer.clear(kBackground);
/// renderer.drawRectangle(size, color, placement);
/// renderer.endFrame();   // presents what was drawn
/// ```
///
/// Implementations own their graphics context. They never query an
/// `EntityManager`, never see an entity, and never own one.
class Renderer
{
public:
    virtual ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    /// Starts a frame. Called once before any clear or draw.
    virtual void beginFrame() = 0;

    /// Sets the colour the frame is cleared to.
    virtual void clear(const Color& color) = 0;

    /// Submits one axis-aligned rectangle.
    ///
    /// @param size Width and height in pixels.
    /// @param color Fill colour, channels in [0, 1].
    /// @param placement Where to put it. `placement.position` is the **centre**
    ///        of the rectangle; implementations must honour that, so callers
    ///        never need to reason about primitive-local origins.
    virtual void drawRectangle(const Vec2& size, const Color& color, const RenderTransform& placement) = 0;

    /// Submits one loaded image.
    ///
    /// The whole texture is drawn: there is no source rectangle, because sprite
    /// sheets and frame selection are a later concern and adding a defaulted
    /// rectangle now would mean inventing a convention nothing needs yet.
    ///
    /// The image is **centred** on `placement.position`, exactly as a rectangle
    /// is, and `placement.scale` multiplies its natural size. There is no size
    /// parameter: the size is a property of the image, and passing one would let
    /// a caller contradict the file it loaded.
    ///
    /// The camera has already been applied to `placement`, so this is the same
    /// screen-space contract `drawRectangle` has, and the same `RenderTransform`
    /// carries the camera's zoom in `scale`. No implementation should need to know
    /// a camera exists.
    ///
    /// @param texture A loaded asset. Must not be empty; an empty handle is a
    ///        wiring mistake rather than a thing to draw, and an implementation is
    ///        expected to report it rather than draw nothing.
    virtual void drawTexture(const assets::Texture& texture, const RenderTransform& placement) = 0;

    /// Finishes the frame and presents it to the window.
    virtual void endFrame() = 0;

    /// Frames presented so far. Diagnostics and tests only.
    [[nodiscard]] virtual std::uint64_t frameCount() const noexcept = 0;

protected:
    Renderer() = default;
};

} // namespace engine::graphics
