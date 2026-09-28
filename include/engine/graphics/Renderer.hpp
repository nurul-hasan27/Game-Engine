#pragma once

#include "engine/Color.hpp"
#include "engine/assets/Texture.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/math/IntRect.hpp"
#include "engine/math/Vec2.hpp"

#include <cstdint>
#include <optional>

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

    /// Submits one loaded image, or one region of it.
    ///
    /// `source` selects **which part of the image is drawn**, in whole pixels from
    /// its top-left corner. `std::nullopt` means the whole image, which is the
    /// ordinary case and the one every caller that is not animating anything uses.
    ///
    /// ### Why an optional rather than two methods
    ///
    /// "Draw this image" and "draw this part of this image" are the same operation
    /// with a different source, so they are one method. Two overloads would mean two
    /// virtual functions to implement, and an implementation that got them out of
    /// step would draw a sprite and an animation of it slightly differently - which
    /// is the class of bug that only shows up once something is animating.
    ///
    /// The alternative, a sentinel rect meaning "everything", was rejected because a
    /// zero-sized rect is a perfectly ordinary thing to pass and overloading it to
    /// mean the opposite of what it says is exactly the kind of cleverness that
    /// costs someone an afternoon. [engine::kWholeImage](engine/math/IntRect.hpp)
    /// exists for a different reason and is not accepted here; see `isEmpty`.
    ///
    /// ### Centring applies to the drawn region
    ///
    /// The image, or the selected region of it, is **centred** on
    /// `placement.position` - exactly as a rectangle is. So animating a sprite does
    /// not move it: each frame is centred on the same point, and only the region
    /// changes. This is the same convention the course requires for an animation
    /// frame, where the sprite's origin sits at half the frame rather than half the
    /// texture, and getting it wrong would make a character drift sideways as it
    /// runs.
    ///
    /// `placement.scale` multiplies the region's size, not the whole image's.
    ///
    /// The camera has already been applied to `placement`, so this is the same
    /// screen-space contract `drawRectangle` has, and the same `RenderTransform`
    /// carries the camera's zoom in `scale`. No implementation should need to know
    /// a camera exists.
    ///
    /// @param texture A loaded asset. Must not be empty; an empty handle is a
    ///        wiring mistake rather than a thing to draw, and an implementation is
    ///        expected to report it rather than draw nothing.
    /// @param source Which part of the image to draw, or `std::nullopt` for all of
    ///        it. An empty rect - zero width or height - draws nothing and is not
    ///        an error, because an animation asked for a frame it does not have
    ///        should not be able to crash the renderer.
    virtual void drawTexture(const assets::Texture& texture, const RenderTransform& placement,
                             const std::optional<IntRect>& source) = 0;

    /// Finishes the frame and presents it to the window.
    virtual void endFrame() = 0;

    /// Frames presented so far. Diagnostics and tests only.
    [[nodiscard]] virtual std::uint64_t frameCount() const noexcept = 0;

protected:
    Renderer() = default;
};

} // namespace engine::graphics
