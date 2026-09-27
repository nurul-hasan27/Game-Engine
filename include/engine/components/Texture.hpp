#pragma once

#include <string>

namespace engine::components
{

/// Draws an entity using a loaded asset instead of a filled rectangle.
///
/// This is **pure data**, like every other component. It holds the name of an
/// asset declared in the configuration file, and nothing else.
///
/// ### Why a name and not a handle
///
/// The field is `std::string`, deliberately. It is **not** an
/// `assets::Texture` handle, not an index into some array, and not a pointer.
///
/// - A name is self-describing. Reading a level file or a debug dump tells you
///   what the entity is drawn with without a lookup table alongside it.
/// - A name is data. It can be written in a level file, compared, searched and
///   serialised, which an opaque handle or an index cannot be.
/// - A name keeps the ECS free of the graphics library. If the component held a
///   handle, or anything reached from one, every entity and every system that
///   mentioned it would be touching a resource type rather than plain data.
///
/// The handle itself belongs to the `AssetManager`, which loads every asset once
/// and hands out references to them. `RenderSystem` resolves the name to a handle
/// per frame and passes that to the renderer, so the resolution happens in one
/// place instead of being spread across every entity that draws something.
///
/// ### What this component does not hold
///
/// No texture size: the size belongs to the image, and the renderer reads it from
/// the loaded texture rather than having it duplicated here where the two could
/// disagree. No tint: no caller has asked to tint a texture, and an unused colour
/// field is a field that can be set to something meaningless. No frame index or
/// source rectangle: sprite sheets are a later concern and adding the field now
/// would mean deciding the sub-rectangle convention before anything needs it. No
/// layer, z-order or anchor: there is no sorting in this engine, and inventing an
/// ordering scheme here would put it in the one place that cannot express it.
///
/// ### Draw order
///
/// An entity with this component is drawn by `RenderSystem`'s second query,
/// after its rectangle query. See `RenderSystem` for why that ordering is what it
/// is. Within that query, order is ECS iteration order, that is creation order.
class Texture
{
public:
    /// Name of the asset to draw, exactly as declared in the configuration file.
    ///
    /// Compared exactly, so case and spacing matter. A name that is not declared
    /// is **not** silently ignored: `RenderSystem` lets the lookup's
    /// `AssetNotFoundError` propagate, because an entity that quietly draws
    /// nothing is far harder to notice than one that refuses to run.
    std::string assetName;
};

} // namespace engine::components
