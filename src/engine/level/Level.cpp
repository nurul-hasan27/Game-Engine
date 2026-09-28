#include "engine/level/Level.hpp"

#include "engine/level/LevelGrid.hpp"

#include <algorithm>

namespace engine::level
{

namespace
{

/// A cell-anchored record's right edge, in cells.
///
/// A tile is a whole cell wide, so its right edge is one cell past its column.
[[nodiscard]] float cellsWideForCellAnchored(const float gridX) noexcept
{
    return gridX + 1.0F;
}

/// A pixel position expressed in cells.
///
/// The two record kinds do not agree on units, which is the single most surprising
/// thing about this format and the easiest thing to get wrong: a `Tile`'s
/// coordinates are grid units and a `Dec`'s are pixels. Sizing a world from the
/// records means mixing them, so the conversion happens here - once - rather than
/// at each of the four call sites.
///
/// It was written without this at first, and the result was a 700-cell-tall world
/// demanded by a cloud 700 pixels up. The game printed a warning about a level
/// overflowing a 16-cell world, which is how the mismatch was found.
[[nodiscard]] float cellsForPixels(const float pixels) noexcept
{
    return pixels / LevelGrid::kCellSize;
}

} // namespace

float Level::requiredCellsTall() const noexcept
{
    float tallest = 0.0F;

    // A tile occupies the cell it names, so the cell's top edge - not its anchor -
    // is what has to fit. One whole cell is added for that, which is also the
    // smallest a level can be and still contain a tile.
    for (const TileRecord& tile : m_tiles)
    {
        tallest = std::max(tallest, tile.gridY + 1.0F);
    }

    for (const DecorationRecord& decoration : m_decorations)
    {
        // Measured up from the bottom in **pixels**, so it is converted before being
        // compared with cell-anchored heights. A decoration's Y is also its centre,
        // so its top is half a sprite higher; that is not accounted for here, and
        // [requiredCellsTall] is documented as a floor rather than an exact extent.
        tallest = std::max(tallest, cellsForPixels(decoration.y));
    }

    if (m_hasPlayer)
    {
        tallest = std::max(tallest, m_player.gridY + 1.0F);
    }

    return tallest;
}

float Level::requiredCellsWide() const noexcept
{
    float widest = 0.0F;

    for (const TileRecord& tile : m_tiles)
    {
        // A tile is centred in its cell, so half of it hangs into each neighbour.
        // That is not a rounding nicety: a tile in the last column draws half a
        // sprite past the edge of the world, and a grid sized without the overhang
        // would clip it.
        widest = std::max(widest, cellsWideForCellAnchored(tile.gridX));
    }

    for (const DecorationRecord& decoration : m_decorations)
    {
        // Pixels again, for the same reason as the height above.
        widest = std::max(widest, cellsForPixels(decoration.x));
    }

    if (m_hasPlayer)
    {
        widest = std::max(widest, cellsWideForCellAnchored(m_player.gridX));
    }

    return widest;
}

void Level::setPlayer(PlayerRecord player)
{
    if (m_hasPlayer)
    {
        return;
    }

    m_player = std::move(player);
    m_hasPlayer = true;
}

} // namespace engine::level
