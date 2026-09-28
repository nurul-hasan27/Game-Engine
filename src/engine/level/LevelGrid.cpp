#include "engine/level/LevelGrid.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace engine::level
{

LevelGrid LevelGrid::withCellsTall(const float cellsTall)
{
    // Finite and positive. A NaN would propagate silently into every position,
    // because every comparison against it is false; an infinity would put the whole
    // level at a Y of negative infinity. Neither is a level, and both produce
    // numbers that look like coordinates until something draws them.
    if (!std::isfinite(cellsTall) || cellsTall <= 0.0F)
    {
        throw std::invalid_argument{"a level grid needs a positive, finite height in cells, but got " +
                                    std::to_string(cellsTall)};
    }

    return LevelGrid{cellsTall};
}

Vec2 LevelGrid::cellBottomLeftOf(const float gridX, const float gridY) const noexcept
{
    // Level space: the corner is simply the cell's own coordinates scaled up.
    // Engine space: the same X, and a Y measured down from the top of the world.
    return Vec2{gridX * kCellSize, heightInPixels() - (gridY * kCellSize)};
}

Vec2 LevelGrid::centreOf(const float gridX, const float gridY, const Vec2 size) const noexcept
{
    // Anchored at the cell's bottom-left corner, standing up in level space, which
    // is subtracting in the flipped axis. See the header for the derivation.
    const float cellX = gridX * kCellSize;
    const float cellY = gridY * kCellSize;

    return Vec2{cellX + (size.x * 0.5F), heightInPixels() - cellY - (size.y * 0.5F)};
}

Vec2 LevelGrid::pointToWorld(const float levelX, const float levelY) const noexcept
{
    // The one and only place a level Y becomes an engine Y. Every record type that
    // has a vertical position goes through here or through `centreOf`, so there is
    // exactly one flip in the engine to reason about.
    return Vec2{levelX, heightInPixels() - levelY};
}

Vec2 LevelGrid::worldToPoint(const Vec2 world) const noexcept
{
    return Vec2{world.x, heightInPixels() - world.y};
}

} // namespace engine::level
