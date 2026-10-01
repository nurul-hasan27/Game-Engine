#include "engine/systems/TileSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Bullet.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Lifetime.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/Tile.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/Entity.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Collision.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace engine::systems
{

namespace
{

using engine::Vec2;
using engine::components::Animation;
using engine::components::Bullet;
using engine::components::Collider;
using engine::components::Lifetime;
using engine::components::Player;
using engine::components::Tile;
using engine::components::TileType;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::EntityManager;
using engine::level::LevelGrid;
using engine::physics::Collision;
using engine::physics::CollisionReport;
using engine::physics::hitCeilingWith;
using engine::physics::isParticipant;
using engine::physics::partnerOf;

/// How far above the question block its coin appears, in world pixels.
///
/// ### Why 64 is exactly the right number, and not an approximation
///
/// A coin this size drawn to one grid cell is [engine::level::LevelGrid::kCellSize] tall, so
/// its centre is 32 from its own bottom edge. A question block is one cell tall too, so its
/// top edge is 32 from its centre.
///
/// ```text
///   question centre   y
///   question top edge y - 32
///   coin centre       y - 64
///   coin bottom edge  y - 64 + 32  =  y - 32
/// ```
///
/// The coin's bottom edge therefore lands exactly on the block's top edge: it is clear of
/// the block, and there is not a pixel of gap wasted. A smaller offset would put the coin
/// inside the block; a larger one would leave it floating. This is the one number in the
/// phase that had to be right rather than merely plausible, and it is derived rather than
/// guessed for exactly that reason.
///
/// The measurement is between **centres**, because [engine::components::Transform]'s
/// `position` is the centre of an entity's box and of its sprite - the rule
/// [engine::level::LevelGrid] documents and the renderer applies. Placing the coin by its
/// top-left corner instead would put it 32 pixels lower and inside the block.
constexpr float kCoinSpawnHeight = 64.0F;

/// How many game frames the coin exists.
///
/// The course's number for the coin a question block produces. [engine::components::Lifetime]
/// explains why it is counted in game frames and what one frame is; [engine::systems::LifetimeSystem]
/// explains why the countdown runs before this system, which is what makes this mean
/// "thirty frames, then gone" rather than "twenty-nine".
constexpr std::uint32_t kCoinLifetimeFrames = 30U;

/// One participant of one collision, reduced to the two ids the report gives.
///
/// A copy rather than a `Collision&` or a `std::pair<EntityId, EntityId>` from the report's
/// own storage: the report is cleared at the top of every physics step, and this vector
/// outlives the walk that filled it.
struct Pair
{
    /// Whoever did the hitting: a bullet, or the player.
    EntityId actor = engine::ecs::kInvalidEntityId;

    /// Whatever was hit. A tile for every case this system acts on, but the system does not
    /// assume that - the third walk finds the tiles, and anything that is not a tile is
    /// simply not there to find.
    EntityId target = engine::ecs::kInvalidEntityId;
};

/// Whether anything in `pairs` names `id` as the thing that was hit from below.
///
/// The interaction matrix in one place: a brick breaks for a bullet **or** for a player
/// arriving at its underside; a question block is used for the second and not the first.
[[nodiscard]] bool hitFromBelow(const std::vector<Pair>& pairs, const EntityId id) noexcept
{
    for (const Pair& pair : pairs)
    {
        if (pair.target == id)
        {
            return true;
        }
    }

    return false;
}

/// Points an animation component at `assetName`, restarting it from the first frame.
///
/// `repeat` is a parameter rather than a constant because the two callers want opposite
/// things from it and both have a reason:
///
/// - An **explosion** plays once. [engine::components::Animation]'s `repeat` is per entity
///   precisely so that "plays once and is then removed" can be expressed, and
///   [engine::systems::AnimationSystem] removes the entity when a non-repeating animation
///   reports `ended`. It does **not** mean this system destroys anything.
/// - A **used question block** repeats, exactly as the ground and the pipe do. It is
///   ordinary level geometry from this moment on, and a non-repeating animation would delete
///   the block out of the level.
///
/// `ended` is reset as well, because it belongs to the *previous* animation: carried across,
/// it would make the new animation report itself finished on the frame it was selected. The
/// current frame and the tick counter are reset for the same reason - a block that had been
/// halfway through being an explosion would otherwise start its new picture halfway through.
void pointAnimationAt(Animation& animation, const std::string_view assetName, const bool repeat)
{
    animation.assetName = std::string{assetName};
    animation.currentFrame = 0U;
    animation.ticksOnFrame = 0U;
    animation.repeat = repeat;
    animation.ended = false;
}

/// Turns a destroyed brick into its explosion and stops it being solid.
///
/// ### The collider goes; the sprite stays
///
/// [engine::components::Collider] is removed, which is the whole of "the brick stops
/// blocking things". The entity keeps its [engine::components::Transform] and its
/// [engine::components::Animation] - pointed at the explosion strip - because the explosion
/// has to be *seen*, and an entity destroyed here would take the effect with it.
///
/// The removal is also what prevents the brick from being found by the physics pass again,
/// since [engine::systems::PhysicsSystem] queries `<Transform, Collider, Body>`: without a
/// collider the brick is not collidable at all, and there is nothing to switch off.
///
/// ### It is not destroyed here
///
/// [engine::systems::AnimationSystem] destroys the entity when the explosion reports itself
/// ended - which, because [engine::components::Animation] only marks `ended` once the final
/// frame has been held for its full period, is later than the frame the last frame appears on.
/// Destroying it in this system would delete the effect on its first frame and leave no
/// observable animation at all.
///
/// ### The Body is left alone, on purpose
///
/// A static [engine::components::Body] on an entity with no collider does nothing: the
/// physics step's integration loop zeroes its velocity every frame, which is the correct
/// behaviour for something that is not going to move, and it keeps this method to one change
/// rather than two that have to be reasoned about together.
void startExplosion(Entity& brick, Animation& animation, Tile& tile)
{
    tile.activated = true;
    pointAnimationAt(animation, components::kExplosionAnimationName, false);
    static_cast<void>(brick.tryRemoveComponent<Collider>());
}

/// Uses a question block: it becomes the used block, and a coin appears above it.
///
/// ### The transition, not a replacement
///
/// The block keeps its identity, position and collider. Only [engine::components::Tile]'s
/// `type` changes, and [engine::components::Animation] is pointed at the used artwork - one
/// entity through both states, not a new entity replacing an old one.
///
/// That is what makes "the same world position stays associated with the block" true by
/// construction rather than by copying a position and hoping it matches. It is also what
/// [engine::components::TileType]'s [TileType::Question2] value is *for*: a separate state
/// rather than a boolean, so a used block can be recognised as a used block.
///
/// ### The collider does not change
///
/// The box stays whatever the block's animation gave it when it was loaded. The used artwork
/// is a different size from the unused artwork - the library's are 160x160 against 360x360 -
/// and resizing the collider to match would mean a block that **grew or shrank** the moment it
/// was used, in the middle of a level the player is standing in. Gameplay state changing is
/// the rule; geometry changing as a side effect of it is not.
void useQuestionBlock(Animation& animation, Tile& tile, const Vec2& coinPosition, std::vector<Vec2>& coinSpawns)
{
    tile.activated = true;
    tile.type = TileType::Question2;

    // Read back out of the shared table rather than repeating the string, so the loader that
    // decided what a question block is and this line cannot come to disagree about which
    // artwork is the used one.
    const std::string_view used = components::animationFor(TileType::Question2);
    if (!used.empty())
    {
        pointAnimationAt(animation, used, true);
    }

    coinSpawns.push_back(coinPosition);
}

} // namespace

void TileSystem::update(EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds)
{
    // Nothing here reads input or elapsed time. The system's entire input is the frame's
    // collision report, and accepting the two parameters it does not use is what keeps every
    // system in the engine on one signature.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    const CollisionReport& report = *m_collisions;

    // ---- 1. Every bullet's collisions, as id pairs ------------------------------
    //
    // Copied out of the report rather than read from it repeatedly, for one reason: the
    // report belongs to the physics system and describes *this frame*. The later walks
    // mutate the world - colliders come off, animations change - and reading a
    // `Collision` after that would be reading a record whose overlaps no longer describe
    // anything. Two ids cannot go stale, because [engine::ecs::EntityManager] never reuses
    // one.
    //
    // A bullet is destroyed on **any** collision it takes part in, and that is the rule the
    // course gives: its reference bounces a bullet off a tile and destroys both entities on
    // any other partner. Destroying is safe here, mid-walk, because
    // [engine::ecs::EntityManager::destroyEntity] only sets a flag - the entity stops
    // matching immediately and is erased later by the owner. The third walk still finds this
    // bullet's hits, because it reads `bulletHits` and not the world.
    std::vector<Pair> bulletHits;
    for (auto&& [entity, transform, marker] : entities.query<Transform, Bullet>())
    {
        static_cast<void>(transform);
        static_cast<void>(marker);

        bool struck = false;
        for (const Collision& collision : report.collisions())
        {
            if (!isParticipant(collision, entity.id()))
            {
                continue;
            }

            bulletHits.push_back(Pair{entity.id(), partnerOf(collision, entity.id())});
            struck = true;
        }

        if (struck)
        {
            entities.destroyEntity(entity);
        }
    }

    // ---- 2. The player's hits that came from below ------------------------------
    //
    // [engine::physics::hitCeilingWith] is the whole test, and it is Phase 17's rather than
    // something written here: the player was beside the block and *not* vertically
    // overlapping it, is penetrating it now, and was pushed **downwards** - which, in a
    // downward Y axis, is what arriving at the underside of something means.
    //
    // The three things that must *not* trigger a tile are therefore excluded by the same
    // three clauses, and excluded by arithmetic on numbers the collision pass already
    // produced rather than by a threshold somebody tuned:
    //
    //   landing on top     the push was upward, so `hitCeilingWith` is false
    //   a wall             it was already vertically overlapping, so the previous overlap
    //                     was positive and the test is false
    //   a corner           the resolution went sideways, so the push is not downward
    //
    // No distance, no velocity sign, no pixel count. Every clause is a comparison of a
    // quantity physics computed.
    std::vector<Pair> hitsFromBelow;
    for (auto&& [entity, transform, player] : entities.query<Transform, Player>())
    {
        static_cast<void>(transform);
        static_cast<void>(player);

        for (const Collision& collision : report.collisions())
        {
            if (!isParticipant(collision, entity.id()))
            {
                continue;
            }

            if (hitCeilingWith(collision, entity.id()))
            {
                hitsFromBelow.push_back(Pair{entity.id(), partnerOf(collision, entity.id())});
            }
        }
    }

    // ---- 3. The tiles -------------------------------------------------------------
    //
    // One walk, and every tile is visited once, which is what makes activation idempotent
    // for free: however many records in this frame name the same brick - a bullet that
    // overlaps two bricks at once, or a player whose box still touches a block it has just
    // been pushed out of - the block is seen once and started once.
    //
    // [engine::components::Tile]'s `activated` then carries that guarantee across frames,
    // which is the only reason it exists.
    std::vector<Vec2> coinSpawns;

    for (auto&& [entity, transform, animation, tile] : entities.query<Transform, Animation, Tile>())
    {
        const EntityId id = entity.id();

        // What bullets reached this tile. Computed for every tile, not only for bricks,
        // because the rule is that **any** gameplay tile stops a bullet - including the
        // plain ground and the pipe, which have no behaviour of their own. A bullet must not
        // pass through anything solid just because the thing it hit does nothing interesting.
        bool struckByBullet = false;
        for (const Pair& pair : bulletHits)
        {
            if (pair.target == id)
            {
                struckByBullet = true;
                break;
            }
        }

        // Already done. A question block that has been used and a brick that is exploding
        // are both finished, whatever else happens to them.
        if (tile.activated)
        {
            continue;
        }

        if (tile.type == TileType::Brick)
        {
            // Two triggers, and they are the course's two: a bullet reaching the brick from
            // anywhere, and the player arriving at it from underneath. Landing on it, or
            // walking into its side, reaches neither and the brick survives.
            //
            // Checked as one predicate rather than as two branches, because "what can break
            // a brick" is a single question and a single list of the answers is harder to get
            // wrong than a chain of `if`s.
            if (struckByBullet || hitFromBelow(hitsFromBelow, id))
            {
                startExplosion(entity, animation, tile);
            }

            continue;
        }

        if (tile.type != TileType::Question)
        {
            continue;
        }

        // A question block is activated by the player arriving at it from below, and by
        // nothing else. A bullet stops at it - the loop above already removed the bullet -
        // but does not use it, because the course specifies the player's hit as the trigger
        // and a rule that said "any hit works" would be a rule nobody chose.
        if (hitFromBelow(hitsFromBelow, id))
        {
            useQuestionBlock(animation, tile, Vec2{transform.position.x, transform.position.y - kCoinSpawnHeight},
                             coinSpawns);
        }
    }

    // ---- 4. The coins, once every walk has finished ---------------------------
    //
    // [engine::ecs::EntityManager::addEntity] invalidates the iterators a
    // [engine::ecs::Query] is holding, so this is after all three walks and not inside one.
    // It is also why the positions were gathered into a vector rather than acted on as they
    // were found.
    for (const Vec2& position : coinSpawns)
    {
        spawnCoin(entities, position);
    }
}

void TileSystem::spawnCoin(EntityManager& entities, const Vec2 position) const
{
    // Resolved through the manager, like every other animation name in the engine. A name
    // that does not resolve throws rather than drawing nothing - and it cannot not resolve,
    // because it is one of the engine's own constants rather than something a level supplied.
    const engine::assets::Animation& definition = m_assets->animation(components::kCoinAnimationName);

    const Vec2 frameSize{static_cast<float>(definition.frameWidth()), static_cast<float>(definition.frameHeight())};

    // ### Why the coin is scaled
    //
    // The library's coin is 500x299 pixels and the grid cell is 64, so drawn 1:1 the coin
    // would be nearly eight cells wide and would swallow the block that dropped it.
    // [engine::components::Transform]'s `scale` is the engine's existing answer to "draw
    // this picture at a different size" - the course's own reference uses it to fit the
    // player's 190x208 sprite into a cell - and it costs nothing here because **the coin has
    // no collider**, so scaling it changes no geometry and no collision.
    //
    // The **longest** side is fitted to the cell rather than each side separately, so the
    // artwork keeps its aspect ratio. Fitting both axes to 64 would stretch a 500x299 coin
    // into a square.
    const float longest = std::max(frameSize.x, frameSize.y);
    const float scale = longest > 0.0F ? (LevelGrid::kCellSize / longest) : 1.0F;

    Entity& coin = entities.addEntity(std::string{components::kCoinTag});

    coin.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, Vec2{scale, scale}, 0.0F});

    // `repeat` true, for the same reason a bullet's is: a one-frame animation that does not
    // repeat is *ended* after a single tick, and [engine::systems::AnimationSystem] would
    // destroy the coin on the frame it was created. The coin's lifetime is thirty frames of
    // [engine::components::Lifetime], not the length of a picture.
    coin.addComponent<Animation>(Animation{std::string{components::kCoinAnimationName}, 0U, 0U, true, false});
    coin.addComponent<Lifetime>(Lifetime{kCoinLifetimeFrames});

    // No [engine::components::Collider] and no [engine::components::Body], on purpose. The
    // course says a coin appears when a question block is hit and nothing about what it then
    // does - no score, no pickup, no sound. So it cannot be collided with, cannot be picked
    // up and cannot fall: it is a picture with a lifetime. A later phase that gives it
    // behaviour adds the components it needs, and this line changes.
}

} // namespace engine::systems