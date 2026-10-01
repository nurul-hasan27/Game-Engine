#pragma once

#include <string_view>

namespace engine::components
{

/// What kind of gameplay tile this is, and therefore what hitting it does.
///
/// ### Why this is a component and not something the animation name says at the point of
/// use
///
/// The course is explicit that behaviour follows from the tile's artwork: *"Tiles have
/// different behavior depending on which Animation they are given"*, and its own sample
/// code prints *"This could be a good way of identifying if a tile is a brick!"* while
/// comparing `animation.getName()` to the string `"Brick"`.
///
/// That is a workable suggestion and a poor implementation, for the reason
/// [engine::components::Bullet](Bullet.hpp) gives in full: an animation name is a
/// *rendering* choice, read as a *gameplay* fact. A level author who gave a brick
/// different artwork would silently turn it into a plain block, and two tiles that happen
/// to share artwork would share behaviour whether or not that was intended.
///
/// So the name is resolved **once**, by [engine::level::LevelLoader], into one of these
/// four values, and the value is what every gameplay system reads. The string comparison
/// still exists - it is simply in one table ([tileTypeFor](Tile.hpp)) instead of scattered
/// through the collision code, where a typo would be a silent behaviour change rather than
/// a compile error.
///
/// ### The four values, and what each one does
///
/// | Value | Hitting it does |
/// | ----- | ---------------- |
/// | [Solid](Tile.hpp) | nothing. The floor, the ground, a pipe |
/// | [Brick](Tile.hpp) | explodes, and stops colliding |
/// | [Question](Tile.hpp) | becomes a [Question2](Tile.hpp), and drops a coin |
/// | [Question2](Tile.hpp) | nothing. It is the used state of a [Question](Tile.hpp) |
///
/// The default is [Solid](Tile.hpp), which is what an unrecognised name maps to. That is
/// the safe direction: a tile nobody recognises is ordinary geometry, which is exactly
/// what the great majority of a level is. The alternative - refusing to load a level with
/// an unknown tile - would make adding a new kind of scenery a change to the asset table
/// as well.
enum class TileType
{
    /// Ordinary level geometry. Collides, does nothing else.
    Solid,

    /// A destructible block. A bullet or a player arriving at it from below starts its
    /// explosion animation and it stops being solid.
    Brick,

    /// A block that has not been used. A player arriving at it from below turns it into a
    /// [Question2](Tile.hpp) and drops a coin above it.
    Question,

    /// A [Question](Tile.hpp) that has already been used. Identical geometry, and no
    /// behaviour at all - which is the whole point of having a separate value rather than
    /// a flag.
    Question2
};

/// A tile's gameplay identity, and whether its one-shot behaviour has already run.
///
/// ### Two fields rather than one
///
/// [type](Tile.hpp) answers "what is this tile?" and [activated](Tile.hpp) answers "has it
/// already done the one thing it does?".
///
/// They are separate because they answer different questions and a single enum cannot
/// carry both. A question block that has been used is still a question block - the course's
/// `Question2` artwork is a used block, not a different kind of thing - so collapsing
/// "used" into the type would mean the type no longer described what the tile *is*. And a
/// brick keeps its `Brick` type for its whole life, including while its explosion plays;
/// what changes is that it is no longer solid, which the absence of its
/// [engine::components::Collider] already says.
///
/// [activated](Tile.hpp) exists for the case both of the others cannot express: **two
/// collisions in the same frame**. A bullet that overlaps two bricks produces two records
/// in [engine::physics::CollisionReport] for the same brick, and the player's box can
/// overlap the same block on consecutive frames while it is being pushed out of it.
/// Without this flag, either of those restarts the explosion and spawns a second coin.
///
/// With it, activation is idempotent by construction: the first record flips the flag and
/// every later one in the same frame - and every frame afterwards - finds it already set.
/// That is a smaller and more reliable rule than any "have I handled this already"
/// bookkeeping, because there is nothing to keep in step.
struct Tile
{
    /// What this tile is. See [TileType](Tile.hpp).
    TileType type = TileType::Solid;

    /// Whether this tile's one-shot behaviour has already run.
    ///
    /// False when it is loaded, and never reset. A used [Question](Tile.hpp) becomes a
    /// [Question2](Tile.hpp) *and* sets this, and an exploding [Brick](Tile.hpp) keeps its
    /// type *and* sets this, so both are protected from a second activation by the same
    /// flag.
    bool activated = false;
};

/// The animation the level declares for a destructible brick.
///
/// ### Why the ground tile, and why that is not a fabrication
///
/// The committed library contains no brick artwork. That is a property of the course's own
/// asset library, and the course's reference resolves it by pointing its `TexBrick`
/// texture at `mario/ground.png`:
///
/// ```text
/// Texture TexBrick ../bin/images/mario/ground.png
/// ```
///
/// So this is the course's own mapping, transcribed into the engine's naming convention
/// rather than invented: a brick is drawn with the ground block's image, is exactly one
/// 64-pixel grid cell, and therefore collides as exactly one cell - which is what makes it
/// placeable in a level at all.
///
/// `assets/assets.txt` carries the same note. Phase 13 declined to declare a Brick
/// animation at this point, on the grounds that nothing had yet established the mapping;
/// the course's `TexBrick` line is that establishment, and this is the phase that needs it.
inline constexpr std::string_view kBrickAnimationName = "mario_Brick_tile";

/// The animation the level declares for an unused question block.
inline constexpr std::string_view kQuestionAnimationName = "mario_question_block";

/// The animation an unused question block becomes once it has been used.
inline constexpr std::string_view kUsedQuestionAnimationName = "mario_question2_block";

/// The animation a [Brick](Tile.hpp) plays when it is destroyed.
///
/// Already declared for a different reason: the course's reference declares its
/// `EXPLOSION` animation as 12 frames of `animations/explosion.png` advancing every 8 game
/// frames, and this engine declares the same strip as `animations_explosion_burst`.
inline constexpr std::string_view kExplosionAnimationName = "animations_explosion_burst";

/// The entity tag given to the coin a [Question](Tile.hpp) drops.
///
/// A coin is not a tile and has no component of its own: it is a sprite with a
/// [engine::components::Lifetime], and the tag is the only way to tell it from any other
/// short-lived thing a later phase might add. Nothing in the engine branches on it.
inline constexpr std::string_view kCoinTag = "combat.coin";

/// The animation a dropped coin is drawn with.
///
/// One frame, and that is a real property of the artwork rather than a placeholder: the
/// library's `mario/coin.png` is a single 500x299 coin, and the six-frame
/// `animations/coinspin.png` strip cannot be sliced into equal columns that isolate one
/// coin (its gutters are 11, 31, 52, 56 and 35 pixels wide, so no frame count lines up -
/// see the block comment on the divisibility rule in `assets/assets.txt`). A one-frame
/// animation is the honest description of a picture that never advances.
inline constexpr std::string_view kCoinAnimationName = "mario_coin_pickup";

/// The gameplay tile a level's animation name asks for.
///
/// [Solid](Tile.hpp) for anything unrecognised, for the reason given on [TileType](Tile.hpp).
///
/// `constexpr`, and so usable in a `switch` and at compile time. The table is four
/// comparisons long and lives next to the component it produces, so a new kind of tile is
/// added in one place rather than by finding every place that guessed.
[[nodiscard]] constexpr TileType tileTypeFor(const std::string_view animationName) noexcept
{
    if (animationName == kBrickAnimationName)
    {
        return TileType::Brick;
    }

    if (animationName == kQuestionAnimationName)
    {
        return TileType::Question;
    }

    if (animationName == kUsedQuestionAnimationName)
    {
        return TileType::Question2;
    }

    return TileType::Solid;
}

/// The animation a tile of `type` is drawn with, or an empty view for
/// [Solid](Tile.hpp).
///
/// The inverse of [tileTypeFor](Tile.hpp) for the three types that own their animation, and
/// the reason [engine::systems::TileSystem] does not name `mario_question2_block` itself:
/// the loader decides what a question block is, so the system that turns it into a used
/// block reads the name back out of the same table rather than repeating the string, and
/// the two cannot come to disagree.
[[nodiscard]] constexpr std::string_view animationFor(const TileType type) noexcept
{
    switch (type)
    {
        case TileType::Solid:
            return {};
        case TileType::Brick:
            return kBrickAnimationName;
        case TileType::Question:
            return kQuestionAnimationName;
        case TileType::Question2:
            return kUsedQuestionAnimationName;
    }

    return {};
}

} // namespace engine::components