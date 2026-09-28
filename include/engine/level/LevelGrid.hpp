#pragma once

#include "engine/math/Vec2.hpp"

namespace engine::level
{

/// The course's 64-pixel grid, and the one rule that turns a level file into
/// engine positions.
///
/// ### What the course specifies
///
/// Three separate statements, quoted:
///
/// - *"The 'grid' cells are of size 64 by 64 pixels, and the entity should be
///   positioned such that the bottom left corner of its texture is aligned with
///   the bottom left corner of the given grid coordinate."*
/// - *"The grid starts at (0,0) in the bottom-left of the screen."*
/// - *"This differs from SFML's screen coordinate convention, where (0,0) is at
///   the top-left."* and *"But the Y-axis must be flipped when converting into
///   SFML screen coordinates."*
///
/// Plus one about what the resulting number means: *"The entity's position now
/// represents the center of the entity's sprite and bounding box."*
///
/// ### The two coordinate spaces
///
/// ```text
/// level space                 engine space
/// (0,0) bottom-left           (0,0) top-left
/// +y is UP                    +y is DOWN
/// ```
///
/// So a level file's Y axis has to be flipped, and it cannot be flipped from 0:
/// flipping about zero would send the level off into negative Y. It has to be
/// flipped about the level's **height**, which is why [cellsTall](LevelGrid.hpp)
/// is a constructor argument. `worldY = heightPixels - levelY`.
///
/// ### Why the level height is an argument and not a constant
///
/// **The level format carries no height field.** `Tile`, `Dec` and `Player` all
/// describe positions, none describes the size of the world. So the height is a
/// property the *game* knows and the *engine* cannot: two games sharing one
/// loader need different heights, and a level file is not entitled to dictate the
/// world it is dropped into.
///
/// [Level::requiredCellsTall](Level.hpp) reports the floor a height must clear. It
/// is not the answer on its own, and treating it as one would silently make a
/// short level a short world.
///
/// ### This is deliberately a pure function of its arguments
///
/// No file, no ECS, no asset lookup, no SFML. That is what makes the flip and the
/// centring testable on their own, and it is why the conversion was not written
/// inline inside the spawn loop - a mistake in that arithmetic is invisible if it
/// is buried among component additions, and obvious here.
class LevelGrid
{
public:
    /// The cell size, in pixels. **Fixed by the course**, not configurable.
    ///
    /// Not a constructor parameter and not a setting, on purpose: every level in
    /// the world shares one grid, and a grid that could be resized at runtime would
    /// mean a level file means different things under different settings. The
    /// course says 64, so 64 it is, and there is no path by which it becomes
    /// something else.
    static constexpr float kCellSize = 64.0F;

    /// A grid whose world is `cellsTall` cells high.
    ///
    /// `cellsTall` must be positive. A zero-height or negative-height world has no
    /// consistent flip - every position would map to itself or above the top - and
    /// silently producing one is worse than refusing it, because the result would
    /// look like a plausible level.
    ///
    /// The parameter is in **cells**, not pixels, because that is the unit a level
    /// file thinks in; a caller holding pixels divides by [kCellSize](LevelGrid.hpp).
    [[nodiscard]] static LevelGrid withCellsTall(const float cellsTall);

    /// How many 64-pixel cells tall this grid's world is.
    [[nodiscard]] float cellsTall() const noexcept { return m_cellsTall; }

    /// The world height in pixels: `cellsTall * 64`.
    [[nodiscard]] float heightInPixels() const noexcept { return m_cellsTall * kCellSize; }

    /// The **bottom-left corner** of grid cell `(gridX, gridY)`, in engine pixels.
    ///
    /// In level space that corner is simply `(gridX * 64, gridY * 64)`. The flip
    /// moves it to `height - gridY * 64`, which is the Y of the cell's bottom edge
    /// in a downward axis, so a caller who wants the cell's *top* edge adds a
    /// cell. [centreOf](LevelGrid.hpp) does exactly that and is what the loader
    /// actually uses.
    [[nodiscard]] Vec2 cellBottomLeftOf(const float gridX, const float gridY) const noexcept;

    /// The centre of an entity of `size`, whose **bottom-left corner** sits on the
    /// bottom-left of grid cell `(gridX, gridY)`.
    ///
    /// This is the course's whole anchoring rule in one line, and it is worth
    /// reading as arithmetic rather than as a shape:
    ///
    /// ```text
    /// centreX = gridX * 64 + width / 2
    /// centreY = height - gridY * 64 - height_of_entity / 2
    /// ```
    ///
    /// The X term only adds half the width, because the level grows to the right
    /// and the engine does too - no flip on that axis. The Y term subtracts half
    /// the entity's height, because the cell's bottom edge is the anchor and the
    /// entity stands **up** from it in level space, which after the flip means
    /// subtracting rather than adding. Both halves are the difference between a
    /// level that lines up and one that is off by half a sprite in each direction.
    ///
    /// A tile's `size` is its animation's frame size, because the course says the
    /// bounding box is the animation's size and the sprite and the box are centred
    /// together. A player's is its bounding box, because the level format gives a
    /// player no animation and the box is the only size it has.
    [[nodiscard]] Vec2 centreOf(const float gridX, const float gridY, const Vec2 size) const noexcept;

    /// A point given directly in level pixels becomes a point in engine pixels.
    ///
    /// For the `Dec` record, whose `X`/`Y` fields are positions in pixels rather
    /// than grid coordinates. The entity position is its centre, so this needs no
    /// size: the point *is* the centre.
    [[nodiscard]] Vec2 pointToWorld(const float levelX, const float levelY) const noexcept;

    /// The exact inverse of [pointToWorld](LevelGrid.hpp).
    ///
    /// Present so the flip can be checked against something other than
    /// hand-computed arithmetic: a conversion whose inverse does not recover the
    /// input is wrong, and that is a one-line test rather than a page of
    /// arithmetic. It is the only reason this class has a method for undoing
    /// itself.
    [[nodiscard]] Vec2 worldToPoint(const Vec2 world) const noexcept;

private:
    explicit LevelGrid(const float cellsTall) noexcept : m_cellsTall{cellsTall} {}

    float m_cellsTall;
};

} // namespace engine::level
