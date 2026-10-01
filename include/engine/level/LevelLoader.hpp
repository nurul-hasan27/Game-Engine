#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/level/Level.hpp"
#include "engine/level/LevelGrid.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace engine::level
{

/// A level named an animation the asset configuration does not have.
///
/// Derived from [engine::assets::AssetNotFoundError] rather than from a fresh
/// exception type, and that is the whole design decision: a caller that already
/// handles a missing asset keeps working, and a caller that wants to tell "the
/// level asked for something that does not exist" apart from "the game asked for
/// something that does not exist" can catch this one instead.
///
/// The message adds what the level file can add and an asset lookup cannot: the
/// **line of the level file** that asked for it. A missing texture is a bad asset
/// name; a missing animation is a bad level file, and the two need different
/// people to go and fix them.
class LevelAssetError : public engine::assets::AssetNotFoundError
{
public:
    using engine::assets::AssetNotFoundError::AssetNotFoundError;
};

/// The entity tag given to every spawned tile.
inline constexpr std::string_view kTileTag = "level.tile";

/// The entity tag given to every spawned decoration.
inline constexpr std::string_view kDecorationTag = "level.decoration";

/// The entity tag given to the spawned player.
///
/// The player is the one entity in a level that later phases have to *find*
/// rather than iterate: the camera follows it by tag, and the player system needs
/// to know which entity is the player without a dedicated component that says so.
/// A tag is the engine's existing answer to "which entity is this", so a tag is
/// what this uses.
inline constexpr std::string_view kPlayerTag = "level.player";

/// Turns a parsed [Level] into entities.
///
/// ### The division of work
///
/// ```text
/// file text  ->  parseLevelFile  ->  Level  ->  LevelLoader  ->  entities
///               (this module)                (this module)      (ECS)
/// ```
///
/// Parsing knows nothing about assets or entities, and spawning knows nothing
/// about the text format. The middle value is the seam, which is why
/// [Level] is a plain data holder with no dependencies: a test can build one by
/// hand, or parse one from a string, and reach the loader either way.
///
/// ### What the loader validates, and what it does not
///
/// It **does** resolve every animation name a level mentions - tiles,
/// decorations and the player's bullet animation - through the
/// [engine::assets::AssetManager], and refuses a level naming one that does not
/// exist. That check is the reason the loader is given an asset manager at all:
/// a level that loads and then draws nothing is a bug that takes a screenshot to
/// diagnose, and a missing name is a bug a text editor finds immediately.
///
/// It does **not** decide what a tile's artwork *means*. It does, once, and it is
/// worth being precise about what that is: the course says *"Tiles have different
/// behavior depending on which Animation they are given"*, so a level file's only
/// statement about a brick is the name it gave the brick's animation. The loader
/// resolves that name to an [engine::components::TileType] through one table and
/// puts the answer in [engine::components::Tile]. An unrecognised name is
/// [engine::components::TileType::Solid].
///
/// Every tile is otherwise spawned identically - collision, size from its
/// animation - and no gameplay behaviour happens here. A brick does not explode and a
/// question block does not change in this file; [engine::systems::TileSystem] does
/// that, from this frame's collision report. What the loader does is give the systems
/// something semantic to branch on, once, so that none of them has to compare a string.
///
/// An earlier version of this documentation said the loader did not classify tiles at
/// all and that deciding which was "several phases away". It was the classification that
/// was missing, not the decision: the decision was never this file's to make.
///
/// ### Dependency injection, not a singleton
///
/// The asset manager and the grid arrive as constructor arguments, so a test can
/// drive the loader with a fake asset manager and a chosen level height without a
/// window and without the real configuration. There is no global and no static
/// state, which is the decision this engine made in Lecture 20 and has not
/// revisited since.
class LevelLoader
{
public:
    /// A loader that resolves animation names through `assets`.
    ///
    /// The reference is **borrowed** and must outlive the loader. It is not copied
    /// because an asset manager owns GPU resources and is deliberately
    /// non-copyable, and duplicating one would mean loading every texture twice.
    explicit LevelLoader(const engine::assets::AssetManager& assets) noexcept;

    /// Spawns every tile, then every decoration, then the player.
    ///
    /// In that order deliberately, and the order is the file's own: it keeps the
    /// ECS stable for the query loops, and it means the player exists before the
    /// first frame is drawn rather than partway through a spawn.
    ///
    /// ### Failures are all-or-nothing
    ///
    /// If any animation name does not resolve, this throws [LevelAssetError] and
    /// **no entity from this level has been left in the world** - see
    /// [spawnInto](LevelLoader.hpp) for why that is worth a rollback. A half-spawned
    /// level would render, collide and then stop, which is far harder to diagnose
    /// than a load that refused.
    ///
    /// @param level The parsed level.
    /// @param world Where the entities go.
    /// @param grid The level's grid, which carries the world height the Y flip
    ///        needs.
    /// @return How many entities were created: every tile, every decoration, and
    ///         the player.
    [[nodiscard]] std::size_t spawn(const Level& level, engine::ecs::EntityManager& world,
                                    const LevelGrid& grid) const;

    /// The asset manager this loader resolves names through.
    [[nodiscard]] const engine::assets::AssetManager& assets() const noexcept { return *m_assets; }

private:
    /// Resolves an animation name or throws, quoting the level line that asked.
    ///
    /// One function for all three record kinds, so the message and the behaviour
    /// cannot drift apart between a tile, a decoration and the player's bullets.
    ///
    /// The returned reference is deliberately **not** `[[nodiscard]]`.
    ///
    /// A decoration has no collider, so there is nothing to do with its animation's
    /// size, and the call is made anyway - the name has to resolve whether or not
    /// this record kind has anywhere to put the result, or a typo in a decoration's
    /// name would go unnoticed until something failed to draw. Marking the result
    /// `nodiscard` would force a fake use at those two call sites, or a void cast
    /// that Clang ignores anyway.
    ///
    /// The function's failure channel is the throw, not the return value: a name
    /// that does not resolve never comes back, so there is no error to forget to
    /// check. That is what makes discarding the result honest.
    const engine::assets::Animation& resolve(const std::string_view animationName, const std::size_t lineNumber,
                                             const char* const fieldName, const char* const recordKind) const;

    const engine::assets::AssetManager* m_assets;
};

} // namespace engine::level
