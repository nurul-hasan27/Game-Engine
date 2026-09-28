#pragma once

#include "engine/math/Vec2.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace engine::level
{

/// A `Tile` line: level geometry, which the course says has collision.
///
/// Quoted from the assignment: *"Tile N GX GY - Animation Name N, GX Grid X Pos
/// GX float, GY Grid Y Pos GY float"*, and *"Tiles are Entities that define the
/// level geometry and interact with players"*, and *"Tiles will be given a
/// CBoundingBox equal to the size of the animation"*.
///
/// `gridX` and `gridY` are **grid** coordinates, not pixels. The assignment's
/// note is explicit that it is the `GX`/`GY` fields that are in grid units: the
/// `Dec` record spells its own fields `X` and `Y` and calls them positions. See
/// [LevelGrid] for what "grid" means geometrically.
struct TileRecord
{
    /// The **animation** asset this tile displays, by name.
    ///
    /// An animation and not a texture, because the course lets any tile carry any
    /// animation and the bounding box is the animation's frame size. The name is
    /// resolved through the [engine::assets::AssetManager] when the tile is
    /// spawned, never here: parsing must not need a loaded asset, so a level file
    /// can be parsed and inspected with nothing but a file.
    std::string animationName;

    /// Column, in grid cells from the left.
    float gridX = 0.0F;

    /// Row, in grid cells **up from the bottom**. The level's origin is the
    /// bottom-left, so this grows upwards and has to be flipped into the engine's
    /// downward Y axis.
    float gridY = 0.0F;

    /// 1-based line of the file this record came from.
    ///
    /// Kept so that a failure *after* parsing - a missing animation, say - can
    /// still point at the line to edit. A parse error already knows its own line;
    /// this is what a later failure needs.
    std::size_t lineNumber = 0;
};

/// A `Dec` line: something drawn but never touched.
///
/// Quoted: *"Dec N X Y - Animation Name N, X Position X float, Y Position Y
/// float"*, and *"Decoration entities ('Dec' in a level file) are simply drawn to
/// the screen, and do not interact with any other entities in the game in any
/// way"*, and the load order's *"Add the correct bounding boxes to Tile entities,
/// and no bounding boxes to the Dec entities"*.
///
/// The fields are `X` and `Y`, not `GX` and `GY`, and they are called positions
/// rather than grid positions. That is not a typo in the assignment: the note
/// about grid coordinates is scoped to `(GX, GY)` positions, so a decoration's
/// position is in **pixels**. That is what lets a 70x51 cloud sit where the level
/// designer put it rather than snapping to a 64-pixel cell.
struct DecorationRecord
{
    /// The animation asset this decoration displays, by name.
    std::string animationName;

    /// Centre X in **level pixels**, measured right from the left edge.
    float x = 0.0F;

    /// Centre Y in **level pixels**, measured **up from the bottom**. Still needs
    /// flipping into the engine's downward axis even though it is not a grid
    /// coordinate, because the whole level file lives in the same bottom-left
    /// origin space.
    float y = 0.0F;

    /// 1-based line of the file this record came from.
    std::size_t lineNumber = 0;
};

/// The `Player` line: the level's one piece of player configuration.
///
/// Quoted: *"Player GX/Y CW CH SX SY SM GY B"* with
///
/// | Field | Course name | Meaning |
/// | ----- | ----------- | ------- |
/// | `GX`  | Grid Pos X  | starting column, in grid cells |
/// | `GY`  | Grid Pos Y  | starting row, in grid cells up from the bottom |
/// | `CW`  | BoundingBox W/H | collider width |
/// | `CH`  | BoundingBox W/H | collider height |
/// | `SX`  | Left/Right Speed | horizontal movement speed |
/// | `SY`  | Jump Speed   | initial vertical speed on a jump |
/// | `SM`  | Max Speed    | cap the player must not exceed in x or y |
/// | `GY`  | Gravity      | downward acceleration |
/// | `B`   | Bullet Animation | animation asset used for bullets |
///
/// ### The `GY` collision, and why it is not a problem
///
/// `GY` appears **twice**: once as the player's starting grid row, and once as
/// gravity. The assignment uses one letter for both. It is not ambiguous in
/// practice, because the two never coincide - the starting position is the second
/// field on the line and gravity is the eighth - so the field order resolves it.
/// This struct keeps the two under the names that describe them
/// ([gridY](TileRecord) and `gravity`) precisely so that neither can be assigned
/// the other's meaning by accident, and the parser reads them by position.
///
/// ### What this record deliberately is not
///
/// It is configuration, not behaviour. Nothing here moves the player: the course
/// is explicit that *"All movement logic should be in the movement system"* and
/// that the action system's *"ONLY"* job is setting input variables. So this is
/// data copied straight out of the file and handed to a later phase, and the
/// loader does nothing with it beyond storing it on an entity.
struct PlayerRecord
{
    /// Starting column, in grid cells from the left.
    float gridX = 0.0F;

    /// Starting row, in grid cells **up from the bottom**.
    float gridY = 0.0F;

    /// Collider size, `(CW, CH)` in pixels: *"The player will be given a
    /// CBoundingBox of a size specified in the level file"*.
    Vec2 boundingBoxSize{0.0F, 0.0F};

    /// `SX`, left/right movement speed, in pixels per second.
    float leftRightSpeed = 0.0F;

    /// `SY`, jump speed, in pixels per second.
    float jumpSpeed = 0.0F;

    /// `SM`, the cap the player must not exceed in either axis.
    float maxSpeed = 0.0F;

    /// `GY`, gravity, in pixels per second squared.
    float gravity = 0.0F;

    /// `B`, the animation asset bullets use. Named here, resolved at spawn time.
    ///
    /// The course calls it the *bullet* animation and separately describes a
    /// `'Coin'` animation appearing when a question block is hit. Those are the
    /// same slot in the data, and which artwork fills it is a game decision, so
    /// the loader records the name and does not decide.
    std::string bulletAnimationName;

    /// 1-based line of the file this record came from.
    std::size_t lineNumber = 0;
};

/// A whole parsed level: the tiles, the decorations, and the one player config.
///
/// An owning value with no behaviour and no knowledge of the ECS, the renderer or
/// SFML. `parseLevelFile` produces one, a test can assert on one, and
/// [engine::level::LevelLoader] turns one into entities. Keeping the three
/// separate is what makes that possible: the parser is testable with a string
/// literal, the loader is testable with a hand-built `Level` and a fake asset
/// manager, and neither needs a window.
class Level
{
public:
    /// A level with no tiles, no decorations, and no player. Only reachable by
    /// default construction; a parsed level always has a player, because the
    /// format requires exactly one `Player` line.
    Level() = default;

    /// The level's tiles, in file order.
    [[nodiscard]] const std::vector<TileRecord>& tiles() const noexcept { return m_tiles; }

    /// The level's decorations, in file order.
    [[nodiscard]] const std::vector<DecorationRecord>& decorations() const noexcept { return m_decorations; }

    /// The level's player configuration.
    ///
    /// A value rather than a pointer, because the format guarantees exactly one
    /// `Player` line and a parsed level therefore always has one. A default
    /// constructed `Level` has the zeros, which is why [hasPlayer](Level.hpp) is
    /// offered alongside for code that can legitimately hold an empty one.
    [[nodiscard]] const PlayerRecord& player() const noexcept { return m_player; }

    /// True when this level carries player configuration.
    [[nodiscard]] bool hasPlayer() const noexcept { return m_hasPlayer; }

    /// The number of grid cells the level's tallest thing reaches, counting from
    /// the bottom.
    ///
    /// Derived from the records, not declared: **the level format carries no
    /// height field**, so the engine cannot know how tall a level is without
    /// looking at it or being told. This is the loader looking.
    ///
    /// A game still has to choose its level height, because a height is a
    /// property of the *world* and not of the entities in it - a level whose
    /// highest tile is at row 3 is not a four-cell-tall level, it is a tall level
    /// with a short level in it. [engine::level::LevelGrid] takes the height as an
    /// explicit argument for exactly that reason, and this is offered as a
    /// floor for it rather than as the answer.
    [[nodiscard]] float requiredCellsTall() const noexcept;

    /// The number of grid cells the level's rightmost thing reaches.
    [[nodiscard]] float requiredCellsWide() const noexcept;

    /// Records a tile. Used by the parser; public so a `Level` can also be built
    /// by hand in a test.
    void addTile(TileRecord tile) { m_tiles.push_back(std::move(tile)); }

    /// Records a decoration. See [addTile](Level.hpp) on why these are public.
    void addDecoration(DecorationRecord decoration) { m_decorations.push_back(std::move(decoration)); }

    /// Records the player configuration.
    ///
    /// The second call is ignored and the first kept, so a `Level` cannot end up
    /// with two players. The parser rejects a duplicate `Player` line outright, so
    /// this is belt and braces for a hand-built level rather than a path the
    /// parser uses.
    void setPlayer(PlayerRecord player);

private:
    std::vector<TileRecord> m_tiles;
    std::vector<DecorationRecord> m_decorations;
    PlayerRecord m_player;
    bool m_hasPlayer = false;
};

} // namespace engine::level
