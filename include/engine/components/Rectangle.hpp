#pragma once

#include "engine/Color.hpp"
#include "engine/math/Vec2.hpp"

namespace engine::components
{

/// A solid, axis-aligned coloured rectangle that the renderer can draw.
///
/// This is the engine's second real component and, like `Transform`, it is
/// **pure data**. It holds no SFML type, does not draw itself, and knows
/// nothing about `Renderer`, `RenderSystem`, `EntityManager` or `Time`. The
/// entity's `Transform` supplies where, how big, how rotated and how scaled;
/// this component supplies only what is unique to it, which is its size and its
/// colour.
///
/// There is deliberately no texture, no sprite sheet, no origin offset, no layer
/// and no anchor here. Those are rendering concerns and belong in the rendering
/// layer, or in later phases once something needs them. Notably the rectangle
/// is centred on `Transform.position`; that convention is applied by the
/// renderer, not stored here.
///
/// Draw order is ECS iteration order, that is creation order, because nothing
/// sorts yet. A later rendering phase will decide what sorting is needed.
struct Rectangle
{
    /// Width and height in pixels. The rectangle is drawn centred on the
    /// transform's position.
    Vec2 size{100.0F, 100.0F};

    /// Fill colour, channels in [0, 1].
    Color color{1.0F, 0.35F, 0.2F, 1.0F};
};

} // namespace engine::components
