#include "engine/level/LevelLoader.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Tile.hpp"
#include "engine/components/Transform.hpp"
#include "engine/physics/Aabb.hpp"

#include <functional>
#include <string_view>

namespace engine::level
{

namespace
{

/// A transform at `position` with everything else at its defaults.
///
/// A helper because every spawned entity wants one, and writing the four-field
/// aggregate out seven times is seven chances to leave a scale at 2.
[[nodiscard]] components::Transform transformAt(const Vec2 position) noexcept
{
    return components::Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};
}

/// The colour a player is drawn in until it has an animation of its own.
/// The animation a player is in when it is standing still.
///
/// Named here rather than in the level file because the level format has no field
/// for it. The `Player` record names exactly one animation - the bullet - and the
/// format is the course's, so a field for the player's own three animations would be
/// inventing syntax. The engine knows which artwork it has loaded; a level does not
/// get to ask for a picture that does not exist.
///
/// [engine::systems::PlayerSystem] holds the other two and the state machine that
/// chooses between all three. The name is repeated there rather than shared through a
/// header, because the loader needs the *spawn* picture and the system needs the
/// *mapping*, and those are two different facts that happen to name the same
/// animation today.
///
/// The player used to be drawn as an orange rectangle, and the reason it was is
/// recorded in the test that pinned it: "The level format gives a player no animation
/// - only a bullet animation - so until a later phase supplies one...". This is that
/// later phase. The rectangle is gone rather than kept alongside the sprite, because
/// the renderer's rectangle and animation queries are separate and an entity carrying
/// both is drawn by both.

/// The declaration is one line, and the words above it are the reason it is one line
/// and not three names and a lookup table.
constexpr std::string_view kPlayerStandAnimation = "megaman_megaStand_stand";

/// A looping animation component naming `animationName`.
///
/// Every level entity's animation loops, and the reason is worth stating: a tile
/// that ran out of frames and was then destroyed would delete the floor out from
/// under the player, and Phase 11 already established that a looping animation
/// never reports itself ended, so it is never destroyed by the animation system.
/// `repeat` is therefore not a taste decision here - it is what keeps level
/// geometry alive.
[[nodiscard]] components::Animation loopingAnimation(std::string animationName) noexcept
{
    return components::Animation{std::move(animationName), 0U, 0U, true, false};
}

/// The gameplay tile a level's animation name asks for.
///
/// A three-line call, and the whole of the classification lives behind it.
///
/// ### Why this is here and not in the collision systems
///
/// The course says *"Tiles have different behavior depending on which Animation they
/// are given"* - a brick explodes, a question block changes - and its own sample
/// identifies a brick by comparing the animation's name to a string. So the
/// information genuinely only exists as a name in the level file, and *somewhere* it
/// has to become something a system can branch on.
///
/// This is that somewhere, and it is here for two reasons:
///
/// - It runs **once**, at spawn. Every gameplay system reads
///   [engine::components::Tile] and never compares a string, so no amount of
///   re-typing the comparison can produce a different answer in a different place.
/// - The loader is the only thing that already maps a level's names onto things.
///   It resolves the animation through the asset manager, it chooses the anchor and
///   it knows what a record kind means. Deciding what a *named* tile *is* belongs with
///   the rest of the name handling rather than in three systems that would each have to
///   repeat it.
///
/// An unrecognised name becomes [engine::components::TileType::Solid], which is the
/// safe direction: a tile nobody recognises is ordinary geometry, and a level is mostly
/// ordinary geometry. See [engine::components::Tile] for the whole argument.
[[nodiscard]] components::Tile tileFor(const std::string_view animationName) noexcept
{
    return components::Tile{components::tileTypeFor(animationName), false};
}

} // namespace

LevelLoader::LevelLoader(const engine::assets::AssetManager& assets) noexcept : m_assets{&assets} {}

const engine::assets::Animation& LevelLoader::resolve(const std::string_view animationName,
                                                      const std::size_t lineNumber, const char* const fieldName,
                                                      const char* const recordKind) const
{
    try
    {
        return m_assets->animation(animationName);
    }
    catch (const engine::assets::AssetNotFoundError& error)
    {
        // Re-thrown as a LevelAssetError, which still *is* an AssetNotFoundError, so
        // a caller catching the asset-level exception keeps catching this one. What
        // is added is the part only the level file knows: which line asked, and
        // which field on it. A missing animation is a typo in a level file, and the
        // line number is the difference between finding it and reading the file.
        throw LevelAssetError{"level file line " + std::to_string(lineNumber) + ": " + recordKind +
                              " record's " + fieldName + " '" + std::string{animationName} +
                              "' is not a declared Animation asset; " + error.what()};
    }
}

std::size_t LevelLoader::spawn(const Level& level, engine::ecs::EntityManager& world,
                               const LevelGrid& grid) const
{
    // Every entity this call creates, so a failure partway through can be undone
    // exactly. A missing animation is discovered *midway* - the level parsed
    // perfectly, so the bad name could be the fortieth tile - and throwing at that
    // point with thirty-nine entities already in the world would leave a game that
    // renders a fragment of a level and then stops, with an error that says nothing
    // about the leftovers.
    //
    // Stored as borrows rather than owned copies because `Entity` is deliberately
    // non-copyable: it owns its component storage, and copying one would duplicate
    // that storage into a second identity. Borrows are safe for the length of this
    // call because `EntityManager` guarantees that `addEntity` does not invalidate
    // references to existing entities - it is a `std::deque` for exactly that
    // reason - and the only thing that does invalidate them, `update()`, is called
    // after the list has been consumed.
    std::vector<std::reference_wrapper<const engine::ecs::Entity>> created;
    created.reserve(level.tiles().size() + level.decorations().size() + 1U);

    try
    {
        // Tiles first. The course separates record kinds by behaviour, not by how
        // they are built: a tile is anything that defines geometry and collides, and
        // *which* animation it carries is what decides its special behaviour later.
        // So every tile gets the same components here, sized from its own animation.
        for (const TileRecord& tile : level.tiles())
        {
            const engine::assets::Animation& animation =
                resolve(tile.animationName, tile.lineNumber, "animationName", "Tile");

            // The course sizes a tile's box from its animation: *"Tiles will be given
            // a CBoundingBox equal to the size of the animation"*, spelled out as
            // `tile->getComponent<CAnimation>().animation.getSize()`. That is the same
            // number that positions it, because the sprite and the box are centred
            // together - so it is read once and used for both.
            const Vec2 size{static_cast<float>(animation.frameWidth()),
                            static_cast<float>(animation.frameHeight())};

            engine::ecs::Entity& entity = world.addEntity(std::string{kTileTag});
            created.emplace_back(entity);

            entity.addComponent<components::Transform>(transformAt(grid.centreOf(tile.gridX, tile.gridY, size)));

            // Referenced by name, never by handle: a component names an asset, exactly
            // as it does everywhere else in the ECS.
            entity.addComponent<components::Animation>(loopingAnimation(tile.animationName));

            entity.addComponent<components::Collider>(components::Collider{size});
            entity.addComponent<components::Body>(components::Body{physics::BodyType::Static});

            // The tile's gameplay identity, resolved from its animation name and resolved
            // once. Every tile gets one - a plain tile's answer is `Solid`, which is a fact
            // rather than an absence - so a gameplay system never has to ask whether a tile
            // has been classified, only what it is.
            entity.addComponent<components::Tile>(tileFor(tile.animationName));
        }

        // Decorations next, and this is the loop the course's one instruction is
        // really about: *"Add the correct bounding boxes to Tile entities, and no
        // bounding boxes to the Dec entities."*
        //
        // So a decoration gets a transform and an animation and **nothing else**. No
        // collider, no body. The absence is the behaviour, which is the whole point
        // of the component model: a cloud is drawn because it has a transform and an
        // animation, and cannot be collided with because it has no collider. Nothing
        // in the engine special-cases the word "Dec".
        for (const DecorationRecord& decoration : level.decorations())
        {
            // Resolved even though a decoration has no collider and its size is
            // therefore unused, so a level naming an animation that does not exist
            // fails here rather than quietly drawing nothing for the rest of the game.
            resolve(decoration.animationName, decoration.lineNumber, "animationName", "Dec");

            // A decoration's X/Y are pixel positions and they are its **centre**, not
            // a cell corner, so there is no cell anchoring and no size arithmetic: the
            // course's entity position is the centre of the sprite, and the file says
            // where that centre is. That is what lets a 70x51 cloud sit where the
            // level designer put it instead of snapping to a 64-pixel cell.
            engine::ecs::Entity& entity = world.addEntity(std::string{kDecorationTag});
            created.emplace_back(entity);

            entity.addComponent<components::Transform>(
                transformAt(grid.pointToWorld(decoration.x, decoration.y)));
            entity.addComponent<components::Animation>(loopingAnimation(decoration.animationName));
        }

        // The player last, so it exists before the first frame is drawn.
        if (level.hasPlayer())
        {
            const PlayerRecord& player = level.player();

            // Resolved for the same reason as a decoration's: a level whose bullet
            // animation does not exist is a broken level, and the line that says so
            // is the line worth having.
            resolve(player.bulletAnimationName, player.lineNumber, "bulletAnimationName", "Player");

            // Anchored at the cell's bottom-left and centred on the **bounding box**,
            // not on an animation. A level file gives a player no animation of its own
            // - only a bullet animation - and the course says *"The player's sprite and
            // bounding box are centered on the player's position"*, so the box is the
            // only size available, and the collider is what gameplay collides against.
            // A later phase that gives the player an animation will have to decide
            // whether the two sizes agree; that is not a call this loader can make on
            // the level's behalf.
            const Vec2 size = player.boundingBoxSize;

            // The centre of the collider box, which is the centre of the sprite too.
            // The course says it in one line - "The player's sprite and bounding box
            // are centered on the player's position" - and this is the same `centreOf`
            // the tiles use, so a player and a tile at the same grid coordinate have
            // the same anchor rule. The sprite is **not** cropped to the collider:
            // megaStand is 190x208 and the collider is 40x60, and the honest
            // relationship is that they share a centre, not that one has been sized to
            // the other. An offset chosen to make one screenshot look right would be a
            // lie about where the player is.
            const Vec2 position = grid.centreOf(player.gridX, player.gridY, size);

            engine::ecs::Entity& entity = world.addEntity(std::string{kPlayerTag});
            created.emplace_back(entity);

            entity.addComponent<components::Transform>(transformAt(position));

            // The player is a sprite now, and the rectangle the comment above warned
            // about has been **removed** rather than left alongside. The renderer's
            // rectangle query and its animation query are separate, so an entity
            // carrying both is drawn by both - twice, in two places, with two
            // different sizes.
            entity.addComponent<components::Animation>(loopingAnimation(std::string{kPlayerStandAnimation}));

            entity.addComponent<components::Collider>(components::Collider{size});
            entity.addComponent<components::Body>(components::Body{physics::BodyType::Dynamic});

            // The level's numbers, copied straight through. Nothing here is
            // interpreted: the speeds and the gravity are the player's system to
            // apply, and the bullet name is for whoever spawns bullets.
            entity.addComponent<components::PlayerConfig>(
                components::PlayerConfig{player.leftRightSpeed, player.jumpSpeed, player.maxSpeed, player.gravity,
                                         player.bulletAnimationName});

            // The respawn anchor is the spawn point itself, not a second computation
            // of it. Copying the one position means a respawn restores exactly where
            // the level put the player, and the two can never drift apart.
            //
            // `grounded` starts **false**, not true. The player is placed in the air
            // at its spawn cell and is genuinely airborne until it lands, so claiming
            // otherwise would let it jump on its very first frame from mid-air. The
            // first update corrects it either way; starting honest means the frame
            // before the correction is not a lie.
            entity.addComponent<components::Player>(components::Player{components::PlayerState::Stand, false, false,
                                                                        position});
        }
    }
    catch (...)
    {
        // The rollback. `destroyEntity` takes effect immediately - the entities stop
        // appearing in views and queries - and `update()` erases them, so the world
        // is left holding exactly what it held before this call. Using the engine's
        // own deferred-destruction path rather than reaching into the manager is what
        // keeps this from becoming a second way to remove an entity.
        for (const std::reference_wrapper<const engine::ecs::Entity>& entity : created)
        {
            world.destroyEntity(entity.get());
        }

        world.update();

        throw;
    }

    return created.size();
}

} // namespace engine::level
