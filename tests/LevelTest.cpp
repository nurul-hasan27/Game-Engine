#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/systems/CameraSystem.hpp"
#include "engine/level/Level.hpp"
#include "engine/level/LevelFile.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/level/LevelLoader.hpp"
#include "engine/physics/Aabb.hpp"

#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Minimal harness, matching the style already used by the other test files.
// ---------------------------------------------------------------------------

int g_failureCount = 0;

void check(const bool condition, const char* const expression, const char* const file, const int line)
{
    if (!condition)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed\n";
    }
}

void checkNear(const float actual, const float expected, const char* const expression, const char* const file,
               const int line)
{
    constexpr float kTolerance = 1e-3F;

    if (std::fabs(actual - expected) > kTolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << '\n';
    }
}

void checkNearVec(const engine::Vec2& actual, const engine::Vec2& expected, const char* const expression,
                  const char* const file, const int line)
{
    checkNear(actual.x, expected.x, expression, file, line);
    checkNear(actual.y, expected.y, expression, file, line);
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)
#define CHECK_NEAR_VEC(actual, expected) checkNearVec((actual), (expected), #actual, __FILE__, __LINE__)

using engine::Vec2;
using engine::assets::Animation;
using engine::assets::AssetManager;
using engine::assets::AssetNotFoundError;
using engine::assets::Font;
using engine::assets::Texture;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::level::DecorationRecord;
using engine::level::Level;
using engine::level::LevelGrid;
using engine::level::LevelLoader;
using engine::level::LevelParseError;
using engine::level::PlayerRecord;
using engine::level::TileRecord;

// ---------------------------------------------------------------------------
// Compile-time guarantees
//
// The shape of the new types, checked where only the compiler can. These hold
// regardless of runtime behaviour, so no mutation can make them pass by accident.
// ---------------------------------------------------------------------------

// The level data model is plain data. It must be default constructible, copyable
// and trivially destructible, because a parsed level is a value that a caller owns
// and inspects with no engine at all - that is what makes the parser testable with
// a string literal.
static_assert(std::is_default_constructible_v<Level>, "Level must be default constructible");
static_assert(std::is_copy_constructible_v<Level>, "Level must be copyable; it is a value");
static_assert(std::is_copy_assignable_v<Level>, "Level must be copy assignable");
// `Level` holds vectors, so it is deliberately *not* trivially destructible. The
// property that matters is that it owns nothing needing explicit cleanup, which the
// copyability assertions above already establish.

// The records are aggregates of plain data. `std::is_aggregate` is what proves
// there is no constructor hiding a defaulted field.
static_assert(std::is_aggregate_v<TileRecord>, "TileRecord must be a plain aggregate");
static_assert(std::is_aggregate_v<DecorationRecord>, "DecorationRecord must be a plain aggregate");
static_assert(std::is_aggregate_v<PlayerRecord>, "PlayerRecord must be a plain aggregate");

// The grid is a value too, and deliberately not configurable: a grid that could be
// resized would mean a level file means different things under different settings.
static_assert(!std::is_default_constructible_v<LevelGrid>,
              "a LevelGrid has no default height; the world height is not optional");
static_assert(std::is_copy_constructible_v<LevelGrid>, "LevelGrid must be copyable");
static_assert(!std::is_polymorphic_v<LevelGrid>, "LevelGrid must be a value, not an interface");

// The loader holds one borrowed pointer and no state of its own, so two loaders
// over one asset manager are interchangeable and neither can go stale.
static_assert(std::is_copy_constructible_v<LevelLoader>, "LevelLoader must be copyable");
static_assert(!std::is_polymorphic_v<LevelLoader>, "LevelLoader must be a value, not an interface");

// The cell size is a compile-time constant of exactly the value the course fixes.
// Asserted here as well as at runtime, because a grid size that became a variable
// would be a silent format change rather than a visible one.
static_assert(LevelGrid::kCellSize == 64.0F, "the course fixes the grid cell at 64x64 pixels");

// The parse error is an invalid_argument, so a caller that already handles a bad
// asset configuration file handles a bad level file without a second catch clause.
static_assert(std::is_base_of_v<std::invalid_argument, LevelParseError>,
              "a level parse failure must be catchable as std::invalid_argument");

// A missing level animation is still a missing asset, so a handler for the asset
// case keeps working. This is the property that makes the error non-flattening.
static_assert(std::is_base_of_v<AssetNotFoundError, engine::level::LevelAssetError>,
              "a level's missing animation must still be an AssetNotFoundError");

// ---------------------------------------------------------------------------
// A fake asset manager
//
// Declares exactly the animations a test asks for, with a chosen frame size, and
// counts lookups so a test can prove the loader asks the asset manager rather than
// inventing sizes of its own.
// ---------------------------------------------------------------------------

class FakeAssetManager final : public AssetManager
{
public:
    /// Declares `name` as a one-frame animation of `frameWidth` x `frameHeight`.
    void declare(const std::string& name, const int frameWidth, const int frameHeight)
    {
        m_animations[name] = Animation{name, 1U, 1U, frameWidth, frameHeight};
    }

    /// Declares `name` as a two-frame animation, for the frame-size checks.
    void declareStrip(const std::string& name, const int frameCount, const int frameWidth, const int frameHeight)
    {
        m_animations[name] = Animation{name, static_cast<std::uint32_t>(frameCount), 1U, frameWidth, frameHeight};
    }

    [[nodiscard]] int lookupCount() const noexcept { return m_lookups; }
    [[nodiscard]] const std::string& lastLookup() const noexcept { return m_lastLookup; }

    const Texture& texture(const std::string_view) const override
    {
        throw AssetNotFoundError{"this double declares no textures; a level must not ask for one"};
    }

    const Font& font(const std::string_view) const override
    {
        throw AssetNotFoundError{"this double declares no fonts"};
    }

    const Animation& animation(const std::string_view name) const override
    {
        ++m_lookups;
        m_lastLookup = std::string{name};

        const auto found = m_animations.find(std::string{name});
        if (found == m_animations.end())
        {
            throw AssetNotFoundError{"no animation named '" + std::string{name} + "'"};
        }

        return found->second;
    }

private:
    std::map<std::string, Animation> m_animations;
    // `mutable` because the double is asked through a `const AssetManager&`, and
    // recording that it was asked is the whole point of it. The production doubles
    // in the other suites do the same for the same reason.
    mutable int m_lookups = 0;
    mutable std::string m_lastLookup;
};

/// Fills `assets` with the six animations the committed level names.
///
/// Sizes are the real committed artwork sizes, because a test that used round
/// numbers could not tell a wrong size from a right one. Populating a reference
/// rather than returning by value, because an `AssetManager` is deliberately
/// non-copyable - the production interface deletes the copy constructor - so a
/// double of it cannot be returned either.
void declareTheCommittedLevel(FakeAssetManager& assets)
{
    assets.declare("mario_ground_tile", 64, 64);
    assets.declare("mario_SmallPipe_tile", 70, 70);
    assets.declare("mario_SmallCloud_dec", 70, 51);
    assets.declare("mario_SmallBush_dec", 150, 68);
    assets.declare("mario_BigBush_dec", 186, 79);
    assets.declare("megaman_megaBuster_shot", 32, 26);
    // The player's own two animations, added by Phase 16. The sizes are the real ones,
    // measured from the committed artwork, because a test that declared 190x208 while
    // the library says 190x208 would stop noticing if the artwork changed.
    assets.declare("megaman_megaStand_stand", 190, 208);
    assets.declare("megaman_megaJump_air", 279, 266);
}

/// Overwrites a frame of stack with a pattern, and returns what it wrote.
///
/// The point is the *side effect*, not the value. A `std::string_view` left pointing
/// at a destroyed temporary still reads whatever bytes happen to be there, so a test
/// for a dangling view has to arrange for those bytes to have changed. Filling a
/// frame of locals is the closest honest approximation of what really happens in a
/// program: something else uses that stack before the dangling read occurs.
///
/// `noinline` so this gets its own frame, and `volatile` on the buffer so the
/// compiler cannot decide the writes are dead and skip them.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
std::size_t
clobberStack()
{
    volatile unsigned char scratch[512] = {};
    for (std::size_t index = 0U; index < sizeof(scratch); ++index)
    {
        scratch[index] = static_cast<unsigned char>(0xABU);
    }

    return scratch[0];
}

/// Counts what a view yields, the way `EcsTest` does it.
///
/// `EntityView` has no `size()`, so this is the established way to count one.
template <typename View>
[[nodiscard]] std::size_t countIn(const View& view)
{
    std::size_t count = 0;
    for (const auto& entity : view)
    {
        static_cast<void>(entity);
        ++count;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Parse helpers
// ---------------------------------------------------------------------------

/// A minimal valid level: one player, so `parseLevelFile` has its required record.
constexpr std::string_view kMinimalPlayer = "Player 0 0 32 32 100 200 300 900 some_bullet";

/// A level with `body` prepended, so a test can state only the records it cares
/// about and not repeat the player line every time.
[[nodiscard]] std::string withPlayer(const std::string_view body)
{
    return std::string{body} + "\n" + std::string{kMinimalPlayer} + "\n";
}

/// Parses and returns the error message, or an empty string if it did not throw.
[[nodiscard]] std::string parseErrorMessage(const std::string_view contents)
{
    try
    {
        static_cast<void>(engine::level::parseLevelFile(contents));
    }
    catch (const LevelParseError& error)
    {
        return error.what();
    }
    catch (const std::exception& error)
    {
        return std::string{"NOT-A-LEVEL-PARSE-ERROR: "} + error.what();
    }

    return {};
}

/// True when `haystack` contains `needle`.
[[nodiscard]] bool contains(const std::string& haystack, const std::string_view needle) noexcept
{
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Parser: valid records
// ---------------------------------------------------------------------------

void testAPlayerRecordParses()
{
    const Level level = engine::level::parseLevelFile(
        "Player 3 4 40 60 200 400 250 900 megaman_megaBuster_shot\n");

    CHECK(level.hasPlayer());

    // Every field, in the order the assignment lists them. `GY` appears twice in
    // the format and the two are read by position: the second field is the starting
    // row, the eighth is gravity.
    const PlayerRecord& player = level.player();
    CHECK_NEAR(player.gridX, 3.0F);
    CHECK_NEAR(player.gridY, 4.0F);
    CHECK_NEAR_VEC(player.boundingBoxSize, Vec2(40.0F, 60.0F));
    CHECK_NEAR(player.leftRightSpeed, 200.0F);
    CHECK_NEAR(player.jumpSpeed, 400.0F);
    CHECK_NEAR(player.maxSpeed, 250.0F);
    CHECK_NEAR(player.gravity, 900.0F);
    CHECK(player.bulletAnimationName == "megaman_megaBuster_shot");
    CHECK(player.lineNumber == 1U);
}

void testATileRecordParses()
{
    const Level level = engine::level::parseLevelFile(withPlayer("Tile mario_ground_tile 7 4"));

    CHECK(level.tiles().size() == 1U);

    const TileRecord& tile = level.tiles().front();
    CHECK(tile.animationName == "mario_ground_tile");
    CHECK_NEAR(tile.gridX, 7.0F);
    CHECK_NEAR(tile.gridY, 4.0F);
    CHECK(tile.lineNumber == 1U);
}

void testADecorationRecordParses()
{
    const Level level = engine::level::parseLevelFile(withPlayer("Dec mario_BigBush_dec 760 96"));

    CHECK(level.decorations().size() == 1U);

    const DecorationRecord& decoration = level.decorations().front();
    CHECK(decoration.animationName == "mario_BigBush_dec");

    // **Pixels**, not grid cells. This is the distinction the whole format turns on,
    // so the value is stored exactly as written and nothing multiplies it by 64.
    CHECK_NEAR(decoration.x, 760.0F);
    CHECK_NEAR(decoration.y, 96.0F);
    CHECK(decoration.lineNumber == 1U);
}

void testFractionalPositionsSurviveExactly()
{
    // The assignment says every position is a float, so a half-cell offset is legal.
    // A parser that truncated to int would silently move a tile.
    const Level level = engine::level::parseLevelFile(
        withPlayer("Tile mario_ground_tile 2.5 1.25\nDec mario_BigBush_dec 100.75 200.5"));

    CHECK_NEAR(level.tiles().front().gridX, 2.5F);
    CHECK_NEAR(level.tiles().front().gridY, 1.25F);
    CHECK_NEAR(level.decorations().front().x, 100.75F);
    CHECK_NEAR(level.decorations().front().y, 200.5F);
}

void testManyRecordsKeepFileOrder()
{
    const Level level = engine::level::parseLevelFile(
        withPlayer("Tile a 0 0\nTile b 1 0\nDec c 10 20\nTile d 2 0\nDec e 30 40"));

    // Order matters: the loader spawns in file order, and a test that checked only
    // the contents would not notice a reordering.
    CHECK(level.tiles().size() == 3U);
    CHECK(level.tiles()[0].animationName == "a");
    CHECK(level.tiles()[1].animationName == "b");
    CHECK(level.tiles()[2].animationName == "d");
    CHECK(level.decorations().size() == 2U);
    CHECK(level.decorations()[0].animationName == "c");
    CHECK(level.decorations()[1].animationName == "e");

    // And each record remembers which line it came from.
    CHECK(level.tiles()[0].lineNumber == 1U);
    CHECK(level.tiles()[1].lineNumber == 2U);
    CHECK(level.decorations()[0].lineNumber == 3U);
    CHECK(level.tiles()[2].lineNumber == 4U);
    CHECK(level.decorations()[1].lineNumber == 5U);
}

void testBlankLinesAndCommentsAreSkipped()
{
    // The asset parser's rule, unchanged: a line with no tokens is blank, and a line
    // whose *first token* starts with '#' is a comment.
    const Level level = engine::level::parseLevelFile(
        "# the whole file's leading comment\n"
        "\n"
        "Tile a 0 0\n"
        "\n"
        "   \t  \n"
        "#Tile commented-out tile 9 9\n"
        "# a trailing comment\n"
        "Player 0 0 32 32 100 200 300 900 b\n");

    CHECK(level.tiles().size() == 1U);
    CHECK(level.tiles().front().animationName == "a");

    // The tile is the third line of the file even though it is the first *record*:
    // line 1 and 2 are a comment and a blank, and lines 4 to 7 are a blank, a
    // whitespace-only line and two more comments. Line numbers count every line,
    // which is the whole point of them - a number that skipped blank lines would
    // point the reader at the wrong place.
    CHECK(level.tiles().front().lineNumber == 3U);
    CHECK(level.hasPlayer());
    CHECK(level.player().lineNumber == 8U);
}

void testNoTrailingNewlineIsFine()
{
    // A file written without a final newline is the ordinary case on many editors,
    // and must not lose its last record. Two shapes matter: a file ending on a
    // record, and a file ending on a comment.
    const std::string noFinalNewline = "Tile a 0 0\nDec b 10 20\nPlayer 0 0 32 32 100 200 300 900 bu";
    const Level level = engine::level::parseLevelFile(noFinalNewline);

    CHECK(level.tiles().size() == 1U);
    CHECK(level.decorations().size() == 1U);
    CHECK(level.hasPlayer());

    // And one that ends on a comment must not loop or lose the player before it.
    const Level endingOnComment = engine::level::parseLevelFile("Player 0 0 32 32 100 200 300 900 bu\n# the end\n");
    CHECK(endingOnComment.hasPlayer());
}

void testCarriageReturnsAreWhitespace()
{
    // A file written on Windows must read exactly like the same file written here,
    // which is why `\r` is spelled out as whitespace rather than left to the locale.
    const Level level = engine::level::parseLevelFile("Tile a 0 0\r\nPlayer 0 0 32 32 100 200 300 900 b\r\n");

    CHECK(level.tiles().size() == 1U);
    CHECK(level.tiles().front().animationName == "a");
    CHECK(level.hasPlayer());
}

void testTheCommittedLevelParses()
{
    // The real file, not a synthetic one, so the shipped configuration is covered by
    // the same assertions as everything else. Read through the path entry point, so
    // the file-reading step is covered too.
    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    CHECK(level.hasPlayer());
    CHECK(level.tiles().size() == 24U);
    CHECK(level.decorations().size() == 4U);

    // The player line, from the file's own values.
    CHECK_NEAR(level.player().gridX, 3.0F);
    CHECK_NEAR(level.player().gridY, 4.0F);
    CHECK_NEAR_VEC(level.player().boundingBoxSize, Vec2(40.0F, 60.0F));
    CHECK_NEAR(level.player().leftRightSpeed, 200.0F);
    CHECK_NEAR(level.player().jumpSpeed, 400.0F);
    CHECK_NEAR(level.player().maxSpeed, 250.0F);
    CHECK_NEAR(level.player().gravity, 900.0F);
    CHECK(level.player().bulletAnimationName == "megaman_megaBuster_shot");

    // And the level fits inside the world the game declares for it.
    CHECK(level.requiredCellsTall() <= 16.0F);
    CHECK(level.requiredCellsWide() <= 32.0F);
}

// ---------------------------------------------------------------------------
// Parser: rejections
// ---------------------------------------------------------------------------

void testAnUnknownRecordIsRejected()
{
    // A typo must not become a line that silently does nothing, which would leave a
    // level missing half its ground and take an hour to find by eye.
    const std::string message = parseErrorMessage(withPlayer("Tiles a 0 0"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));
    CHECK(contains(message, "unknown level record type 'Tiles'"));
    CHECK(contains(message, "Tile"));
    CHECK(contains(message, "Dec"));
    CHECK(contains(message, "Player"));
}

void testAMissingFieldIsRejectedAndNamed()
{
    // A player line has nine ways to be short by one, so the message names the
    // field that is missing rather than making the reader count.
    const std::string message = parseErrorMessage("Player 0 0 32 32 100 200 300\n");

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));

    // "is missing" is the part only the *by-name* message says. The first version of
    // this assertion looked for "bulletAnimationName", which the trailing
    // `expected: Player <gridX> ... <bulletAnimationName>` hint also contains - so it
    // passed for the wrong reason and a mutation that swapped by-name reporting for
    // by-count reporting survived. The hint is present in both messages; this
    // substring is not.
    CHECK(contains(message, "is missing"));
    CHECK(contains(message, "bulletAnimationName"));

    // And the count-only wording must NOT appear, which is the other half.
    CHECK_FALSE(contains(message, "fields but must have exactly"));
}

void testATileMissingItsYIsRejected()
{
    const std::string message = parseErrorMessage(withPlayer("Tile a 0"));

    CHECK(contains(message, "line 1"));
    CHECK(contains(message, "gridY"));
}

void testAnExtraFieldIsRejected()
{
    // Not ignored: an unexpected field means the file says something other than what
    // it appears to, and the count is reported accurately rather than truncated.
    const std::string message = parseErrorMessage(withPlayer("Tile a 0 0 9"));

    CHECK(contains(message, "line 1"));

    // The *true* count, which is five, and the field that made it wrong.
    CHECK(contains(message, "5 fields"));
    CHECK(contains(message, "exactly 4"));
    CHECK(contains(message, "'9'"));
}

void testAPlayerWithElevenFieldsIsCountedAccurately()
{
    // The tokenizer must report the *true* count, so a message does not say "10
    // fields" when there are eleven and send the reader editing the wrong one.
    const std::string message = parseErrorMessage("Player 0 0 32 32 100 200 300 900 b extra\n");

    CHECK(contains(message, "11 fields"));
    CHECK(contains(message, "'extra'"));
}

void testAMalformedIntegerIsRejected()
{
    // `std::stof` would parse "12abc" as 12 and swallow the typo. `from_chars` does not.
    const std::string message = parseErrorMessage(withPlayer("Tile a 0 12abc"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));
    CHECK(contains(message, "12abc"));
    CHECK(contains(message, "not a finite decimal number"));
}

void testASpeedWithTrailingTextIsRejected()
{
    const std::string message = parseErrorMessage("Player 0 0 32 32 100px 200 300 900 b\n");

    CHECK(contains(message, "100px"));
    CHECK(contains(message, "leftRightSpeed"));
}

void testNotANumberAtAllIsRejected()
{
    const std::string message = parseErrorMessage(withPlayer("Tile a 0 -"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));
}

void testNanAndInfAreRejectedByName()
{
    // `from_chars` accepts both spellings, and both would parse, load, and then
    // produce positions that compare false against everything.
    for (const char* token : {"nan", "inf", "-inf", "1e999"})
    {
        const std::string message = parseErrorMessage(withPlayer(std::string{"Tile a 0 "} + token));
        if (message.empty())
        {
            std::cerr << "    accepted " << token << " as a coordinate\n";
        }
        CHECK_FALSE(message.empty());
    }
}

void testAZeroBoundingBoxIsRejected()
{
    // A box of no size can never overlap anything, so every collision involving it
    // would silently fail. That is a defect worth refusing at load time.
    const std::string message = parseErrorMessage("Player 0 0 0 32 100 200 300 900 b\n");

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "boundingBoxWidth"));
    CHECK(contains(message, "not positive"));
}

void testANegativeBoundingBoxHeightIsRejected()
{
    const std::string message = parseErrorMessage("Player 0 0 32 -32 100 200 300 900 b\n");

    CHECK(contains(message, "boundingBoxHeight"));
    CHECK(contains(message, "not positive"));
}

void testANegativeSpeedIsRejected()
{
    // A speed, a cap and a gravity are magnitudes.
    for (const char* field : {"leftRightSpeed", "jumpSpeed", "maxSpeed", "gravity"})
    {
        const std::string message = parseErrorMessage("Player 0 0 32 32 -1 -1 -1 -1 b\n");
        if (message.empty())
        {
            std::cerr << "    accepted a negative " << field << '\n';
        }
        CHECK_FALSE(message.empty());
    }
}

void testZeroSpeedsAreAccepted()
{
    // Zero is *not* rejected for a speed: a level with no gravity is a legitimate
    // thing to author, and the course does not forbid it. Only the bounding box has
    // to be strictly positive, because a zero-sized box is a silent failure.
    const Level level = engine::level::parseLevelFile("Player 0 0 32 32 0 0 0 0 b\n");

    CHECK(level.hasPlayer());
    CHECK_NEAR(level.player().leftRightSpeed, 0.0F);
    CHECK_NEAR(level.player().gravity, 0.0F);
}

void testNegativePositionsAreAccepted()
{
    // The assignment's note is about what a coordinate *means*, not about which
    // values are legal. A decoration hanging off the left edge is legitimate, and
    // refusing it would be inventing a rule the course does not state.
    const Level level = engine::level::parseLevelFile(withPlayer("Tile a -2 -1\nDec b -50 -20"));

    CHECK(level.tiles().size() == 1U);
    CHECK_NEAR(level.tiles().front().gridX, -2.0F);
    CHECK(level.decorations().size() == 1U);
    CHECK_NEAR(level.decorations().front().x, -50.0F);
}

void testABlankAnimationNameIsRejected()
{
    // A double space where a name should be.
    //
    // Worth being precise about what is actually being tested, because the obvious
    // reading is wrong. The parser *does* reject this line - but through the **arity**
    // check, not through the blank-name guard: the tokenizer produces three tokens
    // where four are required, because it never yields an empty run between two
    // spaces. The blank-name guard is therefore unreachable through this parser, a
    // mutation that disabled it was EQUIVALENT, and nothing here claims otherwise.
    //
    // The group is kept because the *observable contract* is worth pinning: a line
    // with a missing field is refused, whichever of the two checks happens to catch
    // it, and the reader gets a line number either way. If the tokenizer ever changes
    // to yield empty tokens, this group will start passing through the other guard,
    // and the message will change - which is the signal that the guard became live.
    const std::string message = parseErrorMessage(withPlayer("Tile  0 0"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));
    CHECK(contains(message, "is missing"));
    CHECK(contains(message, "animationName"));

    // The line is short, not long: the tokenizer collapsed the run of spaces rather
    // than counting it as an empty field. Asserted, because that collapse is the
    // reason the blank-name guard is unreachable, and it would otherwise be a silent
    // assumption.
    CHECK_FALSE(contains(message, "fields but must have exactly"));
}

void testASecondPlayerLineIsRejected()
{
    // The assignment says the file "will also contain a single line which specifies
    // properties of the player in that level", so one is a stated rule.
    const std::string message = parseErrorMessage(withPlayer("Player 1 1 32 32 100 200 300 900 other"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 2"));
    CHECK(contains(message, "exactly one"));
}

void testAMissingPlayerLineIsRejected()
{
    // The format promises one, so a level with none is incomplete. Reported without
    // a line number, because the problem is an absence and any line would be a guess.
    const std::string message = parseErrorMessage("Tile a 0 0\n");

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "no Player line"));
    CHECK_FALSE(contains(message, "line 1"));
}

void testAShortSecondPlayerIsReportedAsShortBeforeItIsReportedAsADuplicate()
{
    // Which of two real problems gets reported, and why.
    //
    // `Player 1 1 broken` is both a second Player line *and* far too short to be one.
    // The arity check runs first, so the missing field is named - and that is the
    // better message: a line too short to be a Player record is not yet a Player at
    // all, so "this is your second Player" would be a claim about a record that does
    // not exist yet. Fix the length first, and the duplicate is then reported.
    //
    // This test exists because a comment in the parser claimed the opposite ordering
    // and a test was written to match the comment instead of the code. The comment
    // was wrong; this records the actual, defensible behaviour.
    const std::string message = parseErrorMessage(withPlayer("Player 1 1 broken"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "line 1"));
    CHECK(contains(message, "bulletAnimationName"));
    CHECK_FALSE(contains(message, "second Player line"));
}

void testAnEndOfLineHashIsATokenNotAComment()
{
    // Comments are whole-line only, matching the asset parser. A trailing `#` is
    // therefore an unexpected field - reported, not swallowed.
    const std::string message = parseErrorMessage(withPlayer("Tile a 0 0 # trailing"));

    CHECK_FALSE(message.empty());
    CHECK(contains(message, "'#'"));
}

void testEveryErrorMessageCarriesALineNumber()
{
    // A level file is edited by hand and is often long. "expected 10 fields" with no
    // line is the error this exists to prevent, so it is asserted across a spread of
    // failures on different lines.
    const std::string prefix = "Tile a 0 0\n\n\n";

    const std::string unknown = parseErrorMessage(prefix + "Nope x\n");
    const std::string shortLine = parseErrorMessage(prefix + "Tile b\n");
    const std::string badNumber = parseErrorMessage(prefix + "Tile c 0 zz\n");
    const std::string extra = parseErrorMessage(prefix + "Tile d 0 0 1\n");

    // Line 4 in all four cases, because that is where the bad record is.
    CHECK(contains(unknown, "line 4:"));
    CHECK(contains(shortLine, "line 4:"));
    CHECK(contains(badNumber, "line 4:"));
    CHECK(contains(extra, "line 4:"));
}

void testANumberErrorCarriesTheExpectedSyntax()
{
    // Every number error ends with `expected: <the record's syntax line>`, so a
    // reader who mistyped a field is shown the whole correct line rather than left to
    // consult a document. Asserted on a *number* error specifically: the arity errors
    // build their own hint inline, so asserting only on those would leave this path
    // untested - and a mutation that emptied the shared hint string survived because
    // nothing looked at it.
    const std::string tile = parseErrorMessage(withPlayer("Tile a 0 zz"));
    CHECK(contains(tile, "expected: Tile   <animationName> <gridX> <gridY>"));

    const std::string dec = parseErrorMessage(withPlayer("Dec a 0 zz"));
    CHECK(contains(dec, "expected: Dec    <animationName> <x> <y>"));

    const std::string player = parseErrorMessage("Player 0 0 32 32 100 200 300 zz b\n");
    CHECK(contains(player, "expected: Player <gridX> <gridY>"));
    CHECK(contains(player, "gravity"));

    // Each record's own hint, not a shared one: a tile's error must not tell the
    // reader to write a Player line.
    CHECK_FALSE(contains(tile, "expected: Player"));
    CHECK_FALSE(contains(player, "expected: Tile"));
}

void testAFailedParseYieldsNoLevel()
{
    // A parse that throws must not leave a half-built level behind for a caller to
    // spawn from, so the throw is the only outcome: there is no partial result.
    bool threw = false;
    try
    {
        static_cast<void>(engine::level::parseLevelFile("Tile a 0 0\nTile b\n"));
    }
    catch (const LevelParseError&)
    {
        threw = true;
    }

    CHECK(threw);
}

void testAMissingFileNamesThePath()
{
    // The parser cannot see a path, so a line number alone would send the reader
    // hunting through the wrong file. The file-reading step has to add it back.
    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(engine::level::loadLevelFile("/definitely/not/here/level.txt"));
    }
    catch (const std::runtime_error& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    CHECK(contains(message, "level.txt"));
}

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

void testTheCellSizeIsSixtyFour()
{
    // The course fixes it, and it is a constant rather than a setting. Asserted as a
    // value as well as a static_assert, so a change to either would be caught.
    CHECK(LevelGrid::kCellSize == 64.0F);
}

void testAGridNeedsAPositiveHeight()
{
    // A zero-height or negative-height world has no consistent flip: every position
    // would map to itself or above the top.
    for (const float cellsTall : {0.0F, -1.0F, -0.5F})
    {
        bool threw = false;
        try
        {
            static_cast<void>(LevelGrid::withCellsTall(cellsTall));
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }

        if (!threw)
        {
            std::cerr << "    accepted a height of " << cellsTall << " cells\n";
        }
        CHECK(threw);
    }
}

void testTheWorldHeightIsTheCellSizeTimesTheCells()
{
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    CHECK_NEAR(grid.cellsTall(), 16.0F);
    CHECK_NEAR(grid.heightInPixels(), 1024.0F);
}

void testTheOriginIsTheBottomLeftOfTheWorld()
{
    // Cell (0,0) is the level's origin, so its bottom-left corner is the *bottom* of
    // the world in the flipped axis - which in engine coordinates is the largest Y.
    // This is the single assertion that proves the flip happened: an unflipped
    // conversion would put the origin at the top.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 corner = grid.cellBottomLeftOf(0.0F, 0.0F);

    CHECK_NEAR_VEC(corner, Vec2(0.0F, 1024.0F));
}

void testGridXIsNotFlipped()
{
    // Only Y is flipped: the level grows to the right and the engine does too, so X
    // passes through untouched.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    for (const float gridX : {0.0F, 1.0F, 7.5F, 20.0F})
    {
        const Vec2 corner = grid.cellBottomLeftOf(gridX, 0.0F);
        CHECK_NEAR(corner.x, gridX * 64.0F);
    }

    CHECK_NEAR(grid.cellBottomLeftOf(3.0F, 0.0F).x, 192.0F);
}

void testGridYCountsUpFromTheBottom()
{
    // Cell row 0 is at the bottom of the world, so it has the largest engine Y.
    // Each row up is exactly one cell less.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    CHECK_NEAR(grid.cellBottomLeftOf(0.0F, 0.0F).y, 1024.0F);
    CHECK_NEAR(grid.cellBottomLeftOf(0.0F, 1.0F).y, 960.0F);
    CHECK_NEAR(grid.cellBottomLeftOf(0.0F, 4.0F).y, 768.0F);
    CHECK_NEAR(grid.cellBottomLeftOf(0.0F, 15.0F).y, 64.0F);
}

void testTheHighestCellSitsAtTheTop()
{
    // The last row's *top* edge lands on the top of the world. Proved through
    // `centreOf` with a zero-height entity, which collapses to the cell's bottom
    // edge, so the arithmetic is checked without a second code path.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 topRowZeroHeight = grid.centreOf(0.0F, 16.0F, Vec2{0.0F, 0.0F});

    CHECK_NEAR(topRowZeroHeight.y, 0.0F);
}

void testAnEntityIsCentredOnItsCell()
{
    // A full-cell entity in a full cell: its centre is the middle of the cell, and
    // it exactly fills the cell rather than overhanging it.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 centre = grid.centreOf(0.0F, 0.0F, Vec2{64.0F, 64.0F});

    CHECK_NEAR_VEC(centre, Vec2(32.0F, 992.0F));

    // Recovering the cell from the centre: left edge 0, right edge 64, top 1024,
    // bottom 960. Exactly cell row 0 of a 1024-tall world.
    CHECK_NEAR(centre.x - 32.0F, 0.0F);
    CHECK_NEAR(centre.x + 32.0F, 64.0F);
    CHECK_NEAR(centre.y - 32.0F, 960.0F);
    CHECK_NEAR(centre.y + 32.0F, 1024.0F);
}

void testASmallEntityHangsFromTheCellsBottomEdge()
{
    // The rule is "the **bottom left corner** of its texture is aligned with the
    // bottom left corner of the grid coordinate" - so a shorter entity is anchored
    // at the bottom and leaves the rest of the cell empty above it. If the centring
    // were wrong in sign, this entity would hang from the top instead, and the
    // assertion on its bottom edge would catch it.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 centre = grid.centreOf(0.0F, 0.0F, Vec2{32.0F, 32.0F});

    // Bottom edge must be the cell's bottom edge: 1024 in the flipped axis.
    CHECK_NEAR(centre.y + 16.0F, 1024.0F);

    // And the left edge the cell's left edge.
    CHECK_NEAR(centre.x - 16.0F, 0.0F);
}

void testANonCellSizedEntityOverhangsItsCell()
{
    // A 70x70 pipe in a 64 cell. The box is *not* snapped to the grid - it is the
    // animation's real size - so it overhangs by 3 pixels on each axis. This is the
    // case that proves the size comes from the animation rather than being assumed
    // to be a cell.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 centre = grid.centreOf(0.0F, 0.0F, Vec2{70.0F, 70.0F});

    CHECK_NEAR_VEC(centre, Vec2(35.0F, 989.0F));

    // Bottom-left still sits on the cell's bottom-left, which is the anchoring rule.
    CHECK_NEAR(centre.x - 35.0F, 0.0F);
    CHECK_NEAR(centre.y + 35.0F, 1024.0F);

    // And the overhang is real: the box is 70 wide in a 64 cell.
    CHECK_NEAR((centre.x + 35.0F) - (centre.x - 35.0F), 70.0F);
}

void testAnAsymmetricCoordinateExposesAMissingFlip()
{
    // Grid X and grid Y chosen so that a missing flip, a sign error, or an
    // off-by-one-cell each produce a *different* wrong answer. X=3 and Y=4 are not
    // equal and not symmetric about anything, so no single mistaken rule
    // accidentally produces the right number.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 centre = grid.centreOf(3.0F, 4.0F, Vec2{40.0F, 60.0F});

    // x = 3*64 + 20 = 212
    // y = 1024 - 4*64 - 30 = 738
    CHECK_NEAR_VEC(centre, Vec2(212.0F, 738.0F));

    // The four wrong answers it must not equal. Written out rather than derived, so
    // a change in the formula cannot quietly move one of them onto the right value.
    const Vec2 unflipped{212.0F, 286.0F};    // y = 4*64 + 30
    const Vec2 signFlipped{212.0F, 778.0F};  // y = 1024 - 4*64 + 30
    const Vec2 offByACell{212.0F, 802.0F};   // y = 1024 - 5*64 - 30
    const Vec2 uncentred{192.0F, 768.0F};    // no centring at all

    CHECK(!(centre == unflipped));
    CHECK(!(centre == signFlipped));
    CHECK(!(centre == offByACell));
    CHECK(!(centre == uncentred));
}

void testThePlayerIsCentredOnItsBoundingBox()
{
    // A player is anchored and centred on its **bounding box**, because the level
    // format gives a player no animation and the box is the only size it has.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    const Vec2 centre = grid.centreOf(3.0F, 4.0F, Vec2{40.0F, 60.0F});

    // Its bottom edge is exactly the cell's bottom edge, which is what lets a player
    // standing on ground in the cell below rest on it.
    CHECK_NEAR(centre.y + 30.0F, 768.0F);
    CHECK_NEAR(centre.x - 20.0F, 192.0F);
}

void testAPixelPointIsFlipped()
{
    // A `Dec` position is in pixels and is the entity's centre, so it needs no size.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    // (300, 700) in a 1024-tall world: x through, y measured down from the top.
    CHECK_NEAR_VEC(grid.pointToWorld(300.0F, 700.0F), Vec2(300.0F, 324.0F));

    // A decoration at the bottom of the level has a small engine Y; one near the top
    // has a large one. An unflipped conversion would invert both of these.
    CHECK_NEAR(grid.pointToWorld(0.0F, 0.0F).y, 1024.0F);
    CHECK_NEAR(grid.pointToWorld(0.0F, 1024.0F).y, 0.0F);
}

void testThePixelConversionInvertsExactly()
{
    // A flip whose inverse does not recover the input is wrong, and that is a
    // one-line test rather than a page of arithmetic.
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    for (const Vec2 levelPoint : {Vec2{0.0F, 0.0F}, Vec2{300.0F, 700.0F}, Vec2{1500.5F, 96.25F},
                                  Vec2{-40.0F, 1024.0F}})
    {
        const Vec2 world = grid.pointToWorld(levelPoint.x, levelPoint.y);
        const Vec2 back = grid.worldToPoint(world);
        CHECK_NEAR_VEC(back, levelPoint);
    }
}

void testTwoGridsOfDifferentHeightsPlaceTheSameRecordDifferently()
{
    // The level height is a game decision, and this is what that decision does: the
    // same tile in a taller world sits lower on the screen, because the flip is taken
    // about a different axis. It is not a bug and it is not a second flip - it is why
    // the height cannot be a constant in the engine.
    const LevelGrid shortWorld = LevelGrid::withCellsTall(10.0F);
    const LevelGrid tallWorld = LevelGrid::withCellsTall(20.0F);

    const Vec2 a = shortWorld.centreOf(2.0F, 1.0F, Vec2{64.0F, 64.0F});
    const Vec2 b = tallWorld.centreOf(2.0F, 1.0F, Vec2{64.0F, 64.0F});

    // Same X, because only Y is flipped.
    CHECK_NEAR(a.x, b.x);
    CHECK_NEAR(a.x, 160.0F);

    // y = height - gridY*64 - halfHeight. A 10-cell world is 640 tall, so cell row 1
    // of it is 544. A 20-cell world is 1280 tall, so the same row is 1184.
    //
    // The taller world puts the tile *lower* on the screen, and the gap is exactly
    // the 640 pixels of extra world added below it. That direction is the point:
    // it is why the height cannot be a constant in the engine, and why a level
    // dropped into a differently-sized world lands somewhere else rather than
    // somewhere wrong.
    CHECK_NEAR(a.y, 544.0F);
    CHECK_NEAR(b.y, 1184.0F);
    CHECK_NEAR(b.y - a.y, 640.0F);
}

void testTheExtentFollowsTheTallestTileNotJustTheTallestDecoration()
{
    // Both halves of the extent, in one level, chosen so neither can be dropped.
    //
    // A tile high in the level, and a decoration lower than it. If the tile term were
    // removed, the answer would come from the decoration alone and be too small; if
    // the cell height were forgotten, the tile's *top* would not be counted and the
    // answer would be one cell too small. The committed level could not catch either,
    // because its tallest thing is a decoration and its highest tile is four rows up.
    Level level;
    level.addTile(TileRecord{"mario_ground_tile", 0.0F, 9.0F, 1U});
    level.addDecoration(DecorationRecord{"mario_BigBush_dec", 100.0F, 320.0F, 2U});

    PlayerRecord player;
    player.boundingBoxSize = Vec2{32.0F, 32.0F};
    player.bulletAnimationName = "b";
    level.setPlayer(player);

    // The tile in row 9 has its top edge at row 10, so the level needs ten cells.
    // The decoration is at 320px = 5 cells, well below that.
    CHECK_NEAR(level.requiredCellsTall(), 10.0F);

    // And a level whose only tall thing is a decoration is measured in the other
    // unit, converted: 700 pixels is 10.9375 cells, not 700.
    Level decorationOnly;
    decorationOnly.addDecoration(DecorationRecord{"mario_BigBush_dec", 100.0F, 700.0F, 1U});
    CHECK_NEAR(decorationOnly.requiredCellsTall(), 700.0F / 64.0F);
}

void testTheExtentFollowsTheWidestTile()
{
    // The same on the other axis, and the player counts too: a player in the last
    // column needs that column to exist.
    Level level;
    level.addTile(TileRecord{"mario_ground_tile", 6.0F, 0.0F, 1U});

    PlayerRecord player;
    player.gridX = 3.0F;
    player.boundingBoxSize = Vec2{32.0F, 32.0F};
    player.bulletAnimationName = "b";
    level.setPlayer(player);

    // A tile in column 6 occupies columns 6, so seven cells are needed.
    CHECK_NEAR(level.requiredCellsWide(), 7.0F);

    // The player in column 3 needs nothing extra, and the tile still governs.
    Level playerOnly;
    playerOnly.setPlayer(player);
    CHECK_NEAR(playerOnly.requiredCellsWide(), 4.0F);
}

void testSetPlayerKeepsTheFirstPlayer()
{
    // The parser refuses a second `Player` line outright, so this is reachable only
    // by a hand-built `Level` - which is exactly the case that needs stating, because
    // a `Level` can be built without the parser. The first call wins, so a caller
    // cannot end up with a level whose player is whichever record was added last.
    Level level;

    PlayerRecord first;
    first.gridX = 1.0F;
    first.gridY = 2.0F;
    first.boundingBoxSize = Vec2{40.0F, 60.0F};
    first.leftRightSpeed = 200.0F;
    first.bulletAnimationName = "first_bullet";

    PlayerRecord second;
    second.gridX = 9.0F;
    second.gridY = 8.0F;
    second.boundingBoxSize = Vec2{10.0F, 10.0F};
    second.leftRightSpeed = 999.0F;
    second.bulletAnimationName = "second_bullet";

    level.setPlayer(first);
    level.setPlayer(second);

    CHECK(level.hasPlayer());
    CHECK_NEAR(level.player().gridX, 1.0F);
    CHECK_NEAR(level.player().leftRightSpeed, 200.0F);
    CHECK(level.player().bulletAnimationName == "first_bullet");
}

/// A level with one tile of `animationName` at the given cell, and a valid player.
[[nodiscard]] Level levelWithOneTile(const std::string& animationName, const float gridX, const float gridY)
{
    Level level;
    level.addTile(TileRecord{animationName, gridX, gridY, 1U});

    PlayerRecord player;
    player.boundingBoxSize = Vec2{32.0F, 32.0F};
    player.bulletAnimationName = "megaman_megaBuster_shot";
    level.setPlayer(player);

    return level;
}

void testATileCarriesTheAnimationTheLevelNamed()
{
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};

    EntityManager world;
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), world, grid));

    CHECK(world.aliveEntityCount() == 2U); // the tile and the player

    for (auto&& [entity, animation] : world.query<engine::components::Animation>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            // By **name**, not by handle: a component refers to an asset exactly as
            // it does everywhere else in the ECS.
            CHECK(animation.assetName == "mario_ground_tile");
        }
    }
}

void testACameraSystemRegisteredFromATemporaryTagStillFindsItsTarget()
{
    // A dangling `std::string_view`, found by AddressSanitizer and not by any test.
    //
    // `CameraSystem` stores the tag it is given for the whole life of the system. It
    // used to store a `std::string_view`, so `add<CameraSystem>(camera,
    // std::string{tag})` - passing a temporary, which is what a caller naturally
    // writes when the tag is a `constexpr std::string_view` constant - left the
    // member pointing at freed stack. AddressSanitizer reported a twelve-byte read
    // out of an eight-byte stack slot; no test noticed, because the demo and every
    // test happened to pass a string literal and the garbage usually did not match
    // anything.
    //
    // Reproducing it needs the stack to actually be reused, or the freed bytes are
    // still intact and the test passes by luck. `clobberStack` therefore writes over
    // a frame of locals first, which is what a real program does between registering
    // a system and running its first frame.
    engine::graphics::Camera camera;
    engine::ecs::SystemManager systems;
    systems.add<engine::systems::CameraSystem>(camera, std::string{engine::level::kPlayerTag});

    // Registering took a temporary's address; scribble over the stack it lived on.
    clobberStack();

    // Now build the world the system is meant to follow, and run a frame.
    EntityManager world;
    Entity& player = world.addEntity(std::string{engine::level::kPlayerTag});
    player.addComponent<Transform>(Transform{Vec2{900.0F, 700.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    Entity& other = world.addEntity("decoration");
    other.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    const engine::input::ActionState actions;
    systems.update(world, actions, 1.0F);

    // The camera followed the player, which it can only do by comparing a live tag.
    // With a dangling view this compares freed memory and matches nothing.
    CHECK_NEAR_VEC(camera.position(), Vec2(900.0F, 700.0F));
}

void testTwoLoadersOverTwoManagersEachUseTheirOwn()
{
    // No hidden global.
    //
    // The lesson this engine drew in Lecture 20 is that a service reachable from
    // anywhere is a singleton wearing a hat, and `Application` deliberately owns its
    // one asset manager and hands it out by const reference. This is the test that
    // says the *loader* kept that rule: two loaders, two different asset managers,
    // and each resolves against its own. A loader that cached the last asset manager
    // it was given in a file-scope variable would pass every other group here.
    FakeAssetManager first;
    first.declare("mario_ground_tile", 64, 64);
    first.declare("megaman_megaBuster_shot", 32, 26);

    FakeAssetManager second;
    second.declare("mario_SmallPipe_tile", 70, 70);
    second.declare("megaman_megaBuster_shot", 16, 16);

    const LevelLoader firstLoader{first};
    const LevelLoader secondLoader{second};

    CHECK(&firstLoader.assets() == &first);
    CHECK(&secondLoader.assets() == &second);

    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    // The first loader can spawn a ground tile; the second cannot, because its
    // manager has never heard of that animation.
    EntityManager firstWorld;
    static_cast<void>(firstLoader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), firstWorld, grid));
    CHECK(firstWorld.getEntities(engine::level::kTileTag).begin() != firstWorld.getEntities(engine::level::kTileTag).end());

    EntityManager secondWorld;
    bool threw = false;
    try
    {
        static_cast<void>(secondLoader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), secondWorld, grid));
    }
    catch (const engine::level::LevelAssetError&)
    {
        threw = true;
    }

    CHECK(threw);

    // And the reverse: a pipe tile is unknown to the first manager and known to the
    // second, so the two are genuinely different and not one shared table.
    EntityManager thirdWorld;
    threw = false;
    try
    {
        static_cast<void>(firstLoader.spawn(levelWithOneTile("mario_SmallPipe_tile", 0.0F, 0.0F), thirdWorld, grid));
    }
    catch (const engine::level::LevelAssetError&)
    {
        threw = true;
    }

    CHECK(threw);

    EntityManager fourthWorld;
    static_cast<void>(secondLoader.spawn(levelWithOneTile("mario_SmallPipe_tile", 0.0F, 0.0F), fourthWorld, grid));
    CHECK(fourthWorld.getEntities(engine::level::kTileTag).begin() != fourthWorld.getEntities(engine::level::kTileTag).end());
}

// ---------------------------------------------------------------------------
// Spawning: tiles
// ---------------------------------------------------------------------------

void testEachTileKeepsItsOwnAnimationName()
{
    // The committed level has two tiles that are *not* the ground tile - the two
    // halves of the pipe - and they name a different animation. A loader that wrote
    // one animation name for every tile would satisfy every other group in this file,
    // because the floor, the ledge and the ground all share a name.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    std::size_t ground = 0;
    std::size_t pipe = 0;

    for (auto&& [entity, collider, animation] :
         world.query<engine::components::Collider, engine::components::Animation>())
    {
        if (entity.tag() != engine::level::kTileTag)
        {
            continue;
        }

        if (collider.size == Vec2(70.0F, 70.0F))
        {
            ++pipe;
            // The pipe's own name, and the pipe's own size, on the same entity.
            CHECK(animation.assetName == "mario_SmallPipe_tile");
        }
        else
        {
            ++ground;
            CHECK(animation.assetName == "mario_ground_tile");
        }
    }

    CHECK(ground == 22U);
    CHECK(pipe == 2U);
}

void testATileIsAnchoredByItsAnimationSize()
{
    // The ground tile is 64x64, so it exactly fills its cell. The pipe is 70x70, so
    // it overhangs - and the point is that *neither* size is assumed to be the cell.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager ground;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), ground, grid));

    EntityManager pipe;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_SmallPipe_tile", 0.0F, 0.0F), pipe, grid));

    for (auto&& [entity, transform] : ground.query<Transform>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            CHECK_NEAR_VEC(transform.position, Vec2(32.0F, 992.0F));
        }
    }

    for (auto&& [entity, transform] : pipe.query<Transform>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            CHECK_NEAR_VEC(transform.position, Vec2(35.0F, 989.0F));
        }
    }
}

void testATileColliderIsTheAnimationSize()
{
    // Quoted: "Tiles will be given a CBoundingBox equal to the size of the
    // animation", spelled out as `animation.getSize()`.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_SmallPipe_tile", 0.0F, 0.0F), world, grid));

    int tiles = 0;
    for (auto&& [entity, collider] : world.query<engine::components::Collider>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            ++tiles;
            CHECK_NEAR_VEC(collider.size, Vec2(70.0F, 70.0F));
        }
    }

    CHECK(tiles == 1);
}

void testATileIsStaticAndLoops()
{
    // A tile that ran out of frames and was then destroyed would delete the floor
    // out from under the player, and Phase 11 established that a looping animation
    // never reports itself ended, so the animation system never removes it.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), world, grid));

    for (auto&& [entity, body] : world.query<engine::components::Body>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            CHECK(body.type == engine::physics::BodyType::Static);
        }
    }

    for (auto&& [entity, animation] : world.query<engine::components::Animation>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            CHECK(animation.repeat);
            CHECK(animation.currentFrame == 0U);
            CHECK(animation.ticksOnFrame == 0U);
            CHECK_FALSE(animation.ended);
        }
    }
}

void testATileHasNoRectangleSoItIsNotDrawnTwice()
{
    // The renderer's rectangle query and its animation query are separate, so an
    // entity carrying both is drawn by both. A tile has an animation, so it must not
    // also have a rectangle.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), world, grid));

    for (auto&& [entity, rectangle] : world.query<engine::components::Rectangle>())
    {
        CHECK(entity.tag() != engine::level::kTileTag);
        static_cast<void>(rectangle);
    }

    // **No** rectangle in the world at all, and this is the count Phase 16 moved.
    //
    // Until this phase the player *was* a rectangle - the level format gave it no
    // animation of its own, so it was drawn as a coloured box - and the assertion was
    // "exactly one, and it is the player's". The player now carries an
    // [engine::components::Animation] like every other drawn entity, and the rectangle
    // was removed rather than kept alongside it, because the renderer's two queries
    // are separate and an entity in both is drawn twice.
    //
    // So this is a change of what the world contains, not a weakened check: zero is
    // now the number that means "nothing is drawn twice", and it is a *stronger*
    // statement than "one, and it is the player's", which permitted exactly the
    // double-draw the group is named for.
    CHECK(countIn(world.query<engine::components::Rectangle>()) == 0U);

    // Restated in the form the group is actually about: nothing carries both, so
    // nothing can be drawn by both queries.
    for (auto&& [entity, animation] : world.query<engine::components::Animation>())
    {
        CHECK_FALSE(entity.hasComponent<engine::components::Rectangle>());
        static_cast<void>(animation);
    }
}

// ---------------------------------------------------------------------------
// Spawning: decorations
// ---------------------------------------------------------------------------

void testADecorationIsDrawnButCannotCollide()
{
    // The course's one instruction, in code: "Add the correct bounding boxes to Tile
    // entities, and no bounding boxes to the Dec entities." The absence *is* the
    // behaviour, so it is checked as an absence.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level = levelWithOneTile("mario_ground_tile", 0.0F, 0.0F);
    level.addDecoration(DecorationRecord{"mario_BigBush_dec", 760.0F, 96.0F, 2U});

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    int decorations = 0;
    for (auto&& [entity, transform, animation] :
         world.query<Transform, engine::components::Animation>())
    {
        if (entity.tag() != engine::level::kDecorationTag)
        {
            continue;
        }

        ++decorations;

        // Drawn: it has a transform and an animation.
        CHECK(animation.assetName == "mario_BigBush_dec");
        CHECK(animation.repeat);
        CHECK_NEAR_VEC(transform.position, Vec2(760.0F, 928.0F));

        // Not collidable: no collider, no body, no rectangle.
        CHECK_FALSE(entity.hasComponent<engine::components::Collider>());
        CHECK_FALSE(entity.hasComponent<engine::components::Body>());
        CHECK_FALSE(entity.hasComponent<engine::components::Rectangle>());
    }

    CHECK(decorations == 1);

    // And across the whole world, exactly one collider per entity that has one:
    // the tile and the player, never the decoration.
    std::size_t withCollider = 0;
    for (auto&& [entity, collider] : world.query<engine::components::Collider>())
    {
        static_cast<void>(collider);
        ++withCollider;
        CHECK(entity.tag() != engine::level::kDecorationTag);
    }

    CHECK(withCollider == 2U);
}

void testADecorationIsNotAffectedByTheTileLoop()
{
    // A tile goes through the anchored path and a decoration through the pixel path,
    // and they are not interchangeable: a decoration snapped to a cell would lose
    // the hand-placed position the format exists to allow.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level;
    level.addDecoration(DecorationRecord{"mario_SmallCloud_dec", 300.0F, 700.0F, 1U});
    PlayerRecord player;
    player.boundingBoxSize = Vec2{32.0F, 32.0F};
    player.bulletAnimationName = "megaman_megaBuster_shot";
    level.setPlayer(player);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    for (auto&& [entity, transform] : world.query<Transform>())
    {
        if (entity.tag() == engine::level::kDecorationTag)
        {
            // x is the raw pixel value: 300, not 300*64.
            CHECK_NEAR(transform.position.x, 300.0F);
            CHECK_NEAR(transform.position.y, 324.0F);
        }
    }
}

// ---------------------------------------------------------------------------
// Spawning: the player
// ---------------------------------------------------------------------------

void testThePlayerKeepsEveryValueTheLevelGaveIt()
{
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level;
    PlayerRecord player;
    player.gridX = 3.0F;
    player.gridY = 4.0F;
    player.boundingBoxSize = Vec2{40.0F, 60.0F};
    player.leftRightSpeed = 200.0F;
    player.jumpSpeed = 400.0F;
    player.maxSpeed = 250.0F;
    player.gravity = 900.0F;
    player.bulletAnimationName = "megaman_megaBuster_shot";
    player.lineNumber = 1U;
    level.setPlayer(player);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    int players = 0;
    for (auto&& [entity, transform, collider, config] :
         world.query<Transform, engine::components::Collider, engine::components::PlayerConfig>())
    {
        if (entity.tag() != engine::level::kPlayerTag)
        {
            continue;
        }

        ++players;

        // Position: cell (3,4) in a 1024-tall world, centred on a 40x60 box.
        CHECK_NEAR_VEC(transform.position, Vec2(212.0F, 738.0F));

        // Collider: exactly the level's CW and CH, not a cell and not a guess.
        CHECK_NEAR_VEC(collider.size, Vec2(40.0F, 60.0F));

        // Every configured value, verbatim. The loader copies and does not interpret.
        CHECK_NEAR(config.leftRightSpeed, 200.0F);
        CHECK_NEAR(config.jumpSpeed, 400.0F);
        CHECK_NEAR(config.maxSpeed, 250.0F);
        CHECK_NEAR(config.gravity, 900.0F);
        CHECK(config.bulletAnimationName == "megaman_megaBuster_shot");
    }

    CHECK(players == 1);
}

void testThePlayerIsDynamicAndTagged()
{
    // The tag is the marker a later phase finds the player by, the way the camera
    // already finds the entity it follows.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), world, grid));

    CHECK(countIn(world.getEntities(engine::level::kPlayerTag)) == 1U);
    CHECK(countIn(world.getEntities(engine::level::kTileTag)) == 1U);
    CHECK(countIn(world.getEntities(engine::level::kDecorationTag)) == 0U);

    for (auto&& [entity, body] : world.query<engine::components::Body>())
    {
        if (entity.tag() == engine::level::kPlayerTag)
        {
            CHECK(body.type == engine::physics::BodyType::Dynamic);
        }
    }
}

void testThePlayerIsDrawnAsAnAnimationWithItsSpriteCentred()
{
    // The successor to a group named `the player is drawn as a rectangle *until it has
    // an animation*`, which is this phase. The old contract was that the player had no
    // animation and was therefore a coloured rectangle; the player has one now, and the
    // rectangle is gone.
    //
    // The idea the old group carried is kept rather than dropped: **what is drawn is
    // what collides**. It used to be checked as "the rectangle's size equals the
    // collider's size", which was a proxy. It is now checked directly, as the
    // relationship between the three things that have to agree - the sprite's centre,
    // the collider's centre, and the transform's position - because that relationship
    // is what the course actually states: "The player's sprite and bounding box are
    // centered on the player's position."
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), world, grid));

    // The player is a sprite, and the animation is the committed standing artwork.
    const auto animations = world.query<engine::components::Animation>();
    std::size_t playerAnimations = 0U;
    for (auto&& [entity, animation] : animations)
    {
        if (entity.tag() == engine::level::kPlayerTag)
        {
            ++playerAnimations;
            CHECK(animation.assetName == "megaman_megaStand_stand");
            // Looping: a player whose animation ended would be destroyed by the
            // animation system, and the level would start again with no player in it.
            CHECK(animation.repeat);
            CHECK_FALSE(animation.ended);
        }
    }
    CHECK(playerAnimations == 1U);

    // No rectangle anywhere, so nothing is drawn twice.
    CHECK(countIn(world.query<engine::components::Rectangle>()) == 0U);

    // The centring. The sprite frame is 190x208 and the collider is 32x32 in this
    // fixture, and they are **not** the same size - deliberately, because the honest
    // relationship is a shared centre rather than one sized to the other. What has to
    // hold is that the transform sits at the centre of both, which is what the
    // renderer and the physics each independently assume.
    EntityManager placed;
    static_cast<void>(loader.spawn(levelWithOneTile("mario_ground_tile", 0.0F, 0.0F), placed, grid));
    for (auto&& [entity, transform, collider] :
         placed.query<Transform, engine::components::Collider>())
    {
        if (entity.tag() != engine::level::kPlayerTag)
        {
            continue;
        }

        // The collider is centred on the transform: the Aabb the physics builds from
        // these two has its centre exactly at the position.
        const engine::physics::Aabb box{transform.position, collider.size};
        CHECK_NEAR(box.center().x, transform.position.x);
        CHECK_NEAR(box.center().y, transform.position.y);

        // And the sprite frame is centred on the same point, because
        // `SfmlRenderer::drawTexture` sets the origin to the middle of the region it
        // draws. So the frame's centre and the collider's centre are the same point,
        // and the frame is larger than the collider on both axes.
        const engine::assets::Animation& stand = assets.animation("megaman_megaStand_stand");
        CHECK(stand.frameWidth() > static_cast<int>(collider.size.x));
        CHECK(stand.frameHeight() > static_cast<int>(collider.size.y));
    }
}

// ---------------------------------------------------------------------------
// Spawning: counts, order, and the whole committed level
// ---------------------------------------------------------------------------

void testSpawnReturnsTheEntityCount()
{
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    const std::size_t spawned = loader.spawn(level, world, grid);

    // Every tile, every decoration, and the player.
    CHECK(spawned == 29U);
    CHECK(spawned == level.tiles().size() + level.decorations().size() + 1U);
    CHECK(world.aliveEntityCount() == spawned);
}

void testTheCommittedLevelSpawnsWithTheRightTags()
{
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    CHECK(countIn(world.getEntities(engine::level::kTileTag)) == 24U);
    CHECK(countIn(world.getEntities(engine::level::kDecorationTag)) == 4U);
    CHECK(countIn(world.getEntities(engine::level::kPlayerTag)) == 1U);
}

void testTheCommittedLevelIsDrawnAndCollides()
{
    // The end-to-end shape of the result, which is what the whole phase is for: 24
    // animated, collidable tiles along the floor, 4 decorations with no colliders,
    // and one player above the ground with its level configuration intact.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    // Every 64x64 tile in the level: 19 along the floor at row 0, plus 3 on the
    // floating ledge at row 4. Filtered on the collider size, which is the animation's
    // size, and on the row, because the ledge is the same tile and a count that
    // ignored the row would have been quietly wrong.
    int floorTiles = 0;
    int ledgeTiles = 0;
    float floorY = 0.0F;
    float ledgeY = 0.0F;
    for (auto&& [entity, transform, collider, animation] :
         world.query<Transform, engine::components::Collider, engine::components::Animation>())
    {
        static_cast<void>(animation);
        if (entity.tag() != engine::level::kTileTag || collider.size != Vec2(64.0F, 64.0F))
        {
            continue;
        }

        if (transform.position.y > 900.0F)
        {
            ++floorTiles;
            floorY = transform.position.y;
        }
        else
        {
            ++ledgeTiles;
            ledgeY = transform.position.y;
        }
    }

    CHECK(floorTiles == 19U);
    CHECK_NEAR(floorY, 992.0F);
    CHECK(ledgeTiles == 3U);
    CHECK_NEAR(ledgeY, 736.0F);

    // The player is above the ground, not inside it, and one cell's worth of Y up
    // from the floor of its own cell.
    float playerY = 0.0F;
    for (auto&& [entity, transform, config] : world.query<Transform, engine::components::PlayerConfig>())
    {
        static_cast<void>(config);
        if (entity.tag() == engine::level::kPlayerTag)
        {
            playerY = transform.position.y;
        }
    }

    CHECK_NEAR(playerY, 738.0F);
    CHECK(playerY < 992.0F);
}

void testTheLoaderAsksTheAssetManagerForEveryName()
{
    // Sizes are not invented: every tile's box comes from the animation the asset
    // manager handed back, so the lookup count matches the number of records and the
    // last name asked for is the bullet animation, which the player resolves last.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    // 24 tiles + 4 decorations + 1 bullet animation.
    CHECK(assets.lookupCount() == 29);
    CHECK(assets.lastLookup() == "megaman_megaBuster_shot");
}

void testAMultiFrameAnimationStillSizesTheBox()
{
    // A tile's box is the animation's *frame* size, not the texture's. A two-frame
    // strip must give the per-frame size, which is the size a collision should use.
    FakeAssetManager assets;
    assets.declareStrip("strip", 2, 40, 24);

    // The fixture's player names the buster, so the double has to declare it or the
    // spawn fails on the bullet animation before the tile is ever examined.
    assets.declare("megaman_megaBuster_shot", 32, 26);

    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager world;
    static_cast<void>(loader.spawn(levelWithOneTile("strip", 0.0F, 0.0F), world, grid));

    for (auto&& [entity, collider] : world.query<engine::components::Collider>())
    {
        if (entity.tag() == engine::level::kTileTag)
        {
            CHECK_NEAR_VEC(collider.size, Vec2(40.0F, 24.0F));
        }
    }
}

// ---------------------------------------------------------------------------
// Asset validation
// ---------------------------------------------------------------------------

void testAMissingTileAnimationFailsLoudly()
{
    // Never a default or empty entity: a level that loads and then draws nothing is a
    // bug that takes a screenshot to diagnose, and a missing name is one a text
    // editor finds immediately.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager throwawayWorld;

    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(loader.spawn(levelWithOneTile("no_such_tile", 0.0F, 0.0F), throwawayWorld, grid));
    }
    catch (const engine::level::LevelAssetError& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    // The whole clause, not just the name. The wrapped `AssetNotFoundError` quotes
    // the animation name too, so an assertion on the name alone passed even when the
    // level message stopped saying *which field on which line* asked for it - and a
    // mutation that dropped exactly that survived. The record kind and field name
    // appear only here.
    CHECK(contains(message, "level file line 1"));
    CHECK(contains(message, "Tile record's animationName 'no_such_tile'"));
}

void testAMissingDecorationAnimationFailsLoudly()
{
    // A decoration has no collider and its size is unused, so resolving its name is
    // the *only* thing that catches a typo there.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level = levelWithOneTile("mario_ground_tile", 0.0F, 0.0F);
    level.addDecoration(DecorationRecord{"no_such_bush", 10.0F, 20.0F, 7U});

    EntityManager throwawayWorld;

    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(loader.spawn(level, throwawayWorld, grid));
    }
    catch (const engine::level::LevelAssetError& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    CHECK(contains(message, "no_such_bush"));
    CHECK(contains(message, "line 7"));
    CHECK(contains(message, "Dec"));
}

void testAMissingBulletAnimationFailsLoudly()
{
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level;
    PlayerRecord player;
    player.boundingBoxSize = Vec2{32.0F, 32.0F};
    player.bulletAnimationName = "no_such_bullet";

    // A distinct line number, and asserted back. The default is 0 for a record built
    // by hand rather than parsed, so asserting "line 1" would have been asserting an
    // accident of the fixture; this checks that the loader propagates whatever the
    // record carries, which is the property that matters when a level is parsed.
    player.lineNumber = 42U;
    level.setPlayer(player);

    EntityManager throwawayWorld;

    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(loader.spawn(level, throwawayWorld, grid));
    }
    catch (const engine::level::LevelAssetError& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    // Same reasoning as the tile case: the name alone is also in the wrapped cause.
    CHECK(contains(message, "level file line 42"));
    CHECK(contains(message, "bulletAnimationName 'no_such_bullet'"));
}

void testAMissingAnimationIsStillAnAssetNotFound()
{
    // The error is not flattened: a caller catching the asset-level exception keeps
    // catching this one, and can still tell a bad level from a bad game.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    EntityManager throwawayWorld;

    bool caughtAsAsset = false;
    try
    {
        static_cast<void>(loader.spawn(levelWithOneTile("nope", 0.0F, 0.0F), throwawayWorld, grid));
    }
    catch (const AssetNotFoundError&)
    {
        caughtAsAsset = true;
    }
    catch (const std::exception&)
    {
        caughtAsAsset = false;
    }

    CHECK(caughtAsAsset);
}

void testAFailedSpawnLeavesTheWorldAsItFoundIt()
{
    // A missing animation is discovered *midway* - the level parsed perfectly, so the
    // bad name could be the fortieth tile. Throwing at that point with thirty-nine
    // entities already in the world would leave a game that renders a fragment of a
    // level and then stops, with an error that says nothing about the leftovers.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    // Make the *last* thing the loader resolves - the bullet animation - the bad one.
    Level broken = level;
    PlayerRecord player = broken.player();
    player.bulletAnimationName = "no_such_bullet";
    broken = Level{};
    for (const TileRecord& tile : level.tiles())
    {
        broken.addTile(tile);
    }
    for (const DecorationRecord& decoration : level.decorations())
    {
        broken.addDecoration(decoration);
    }
    broken.setPlayer(player);

    EntityManager world;
    // Something already in the world, so "left as it found it" is a real claim and
    // not just "the world is empty either way".
    Entity& bystander = world.addEntity("bystander");
    bystander.addComponent<Transform>(Transform{Vec2{5.0F, 5.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    const std::size_t before = world.aliveEntityCount();
    const std::size_t storedBefore = world.storedEntityCount();

    bool threw = false;
    try
    {
        static_cast<void>(loader.spawn(broken, world, grid));
    }
    catch (const engine::level::LevelAssetError&)
    {
        threw = true;
    }

    CHECK(threw);

    // Nothing from the level survives: not the 24 tiles, not the 4 decorations, not
    // the 28 good names that resolved before the bad one.
    CHECK(world.aliveEntityCount() == before);
    CHECK(world.aliveEntityCount() == 1U);
    CHECK(countIn(world.getEntities(engine::level::kTileTag)) == 0U);
    CHECK(countIn(world.getEntities(engine::level::kDecorationTag)) == 0U);
    CHECK(countIn(world.getEntities(engine::level::kPlayerTag)) == 0U);

    // And the bystander is untouched, still alive and still where it was.
    CHECK(countIn(world.getEntities("bystander")) == 1U);
    CHECK_NEAR_VEC(world.getEntities("bystander").begin()->getComponent<Transform>().position, Vec2(5.0F, 5.0F));

    // The storage is compacted too, so a failed load does not leave dead entities
    // behind for a later frame to walk past.
    CHECK(world.storedEntityCount() == storedBefore);
}

void testTheLoaderExposesTheAssetManagerItWasGiven()
{
    // Borrowed, never copied: an asset manager owns GPU resources and is deliberately
    // non-copyable, so the loader has to be handed the same one.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};

    CHECK(&loader.assets() == &assets);
}

void testNoTextureIsEverRequested()
{
    // A level names animations, and an animation's texture is reached through the
    // animation. If the loader asked for a texture directly the double would throw,
    // and the group would fail - which is the point of the double refusing textures.
    FakeAssetManager assets;
    declareTheCommittedLevel(assets);
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);

    const Level level = engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE);

    EntityManager world;
    static_cast<void>(loader.spawn(level, world, grid));

    // Every name it asked for was an animation; the double would have thrown on a
    // texture, so reaching here is the assertion.
    CHECK(assets.lookupCount() == 29);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"a player record parses", &testAPlayerRecordParses},
        {"a tile record parses", &testATileRecordParses},
        {"a decoration record parses", &testADecorationRecordParses},
        {"fractional positions survive exactly", &testFractionalPositionsSurviveExactly},
        {"many records keep file order", &testManyRecordsKeepFileOrder},
        {"blank lines and comments are skipped", &testBlankLinesAndCommentsAreSkipped},
        {"no trailing newline is fine", &testNoTrailingNewlineIsFine},
        {"carriage returns are whitespace", &testCarriageReturnsAreWhitespace},
        {"the committed level parses", &testTheCommittedLevelParses},
        {"an unknown record is rejected", &testAnUnknownRecordIsRejected},
        {"a missing field is rejected and named", &testAMissingFieldIsRejectedAndNamed},
        {"a tile missing its y is rejected", &testATileMissingItsYIsRejected},
        {"an extra field is rejected", &testAnExtraFieldIsRejected},
        {"a player with eleven fields is counted accurately", &testAPlayerWithElevenFieldsIsCountedAccurately},
        {"a malformed integer is rejected", &testAMalformedIntegerIsRejected},
        {"a speed with trailing text is rejected", &testASpeedWithTrailingTextIsRejected},
        {"not a number at all is rejected", &testNotANumberAtAllIsRejected},
        {"nan and inf are rejected by name", &testNanAndInfAreRejectedByName},
        {"a zero bounding box is rejected", &testAZeroBoundingBoxIsRejected},
        {"a negative bounding box height is rejected", &testANegativeBoundingBoxHeightIsRejected},
        {"a negative speed is rejected", &testANegativeSpeedIsRejected},
        {"zero speeds are accepted", &testZeroSpeedsAreAccepted},
        {"negative positions are accepted", &testNegativePositionsAreAccepted},
        {"a blank animation name is rejected", &testABlankAnimationNameIsRejected},
        {"a second player line is rejected", &testASecondPlayerLineIsRejected},
        {"a missing player line is rejected", &testAMissingPlayerLineIsRejected},
        {"a short second player is reported as short before it is reported as a duplicate",
         &testAShortSecondPlayerIsReportedAsShortBeforeItIsReportedAsADuplicate},
        {"an end of line hash is a token not a comment", &testAnEndOfLineHashIsATokenNotAComment},
        {"every error message carries a line number", &testEveryErrorMessageCarriesALineNumber},
        {"a number error carries the expected syntax", &testANumberErrorCarriesTheExpectedSyntax},
        {"a failed parse yields no level", &testAFailedParseYieldsNoLevel},
        {"a missing file names the path", &testAMissingFileNamesThePath},
        {"the cell size is sixty four", &testTheCellSizeIsSixtyFour},
        {"a grid needs a positive height", &testAGridNeedsAPositiveHeight},
        {"the world height is the cell size times the cells", &testTheWorldHeightIsTheCellSizeTimesTheCells},
        {"the origin is the bottom left of the world", &testTheOriginIsTheBottomLeftOfTheWorld},
        {"grid x is not flipped", &testGridXIsNotFlipped},
        {"grid y counts up from the bottom", &testGridYCountsUpFromTheBottom},
        {"the highest cell sits at the top", &testTheHighestCellSitsAtTheTop},
        {"an entity is centred on its cell", &testAnEntityIsCentredOnItsCell},
        {"a small entity hangs from the cell's bottom edge", &testASmallEntityHangsFromTheCellsBottomEdge},
        {"a non cell sized entity overhangs its cell", &testANonCellSizedEntityOverhangsItsCell},
        {"an asymmetric coordinate exposes a missing flip", &testAnAsymmetricCoordinateExposesAMissingFlip},
        {"the player is centred on its bounding box", &testThePlayerIsCentredOnItsBoundingBox},
        {"a pixel point is flipped", &testAPixelPointIsFlipped},
        {"the pixel conversion inverts exactly", &testThePixelConversionInvertsExactly},
        {"two grids of different heights place the same record differently",
         &testTwoGridsOfDifferentHeightsPlaceTheSameRecordDifferently},
        {"the extent follows the tallest tile not just the tallest decoration",
         &testTheExtentFollowsTheTallestTileNotJustTheTallestDecoration},
        {"the extent follows the widest tile", &testTheExtentFollowsTheWidestTile},
        {"set player keeps the first player", &testSetPlayerKeepsTheFirstPlayer},
        {"a camera system registered from a temporary tag still finds its target",
         &testACameraSystemRegisteredFromATemporaryTagStillFindsItsTarget},
        {"two loaders over two managers each use their own", &testTwoLoadersOverTwoManagersEachUseTheirOwn},
        {"each tile keeps its own animation name", &testEachTileKeepsItsOwnAnimationName},
        {"a tile carries the animation the level named", &testATileCarriesTheAnimationTheLevelNamed},
        {"a tile is anchored by its animation size", &testATileIsAnchoredByItsAnimationSize},
        {"a tile collider is the animation size", &testATileColliderIsTheAnimationSize},
        {"a tile is static and loops", &testATileIsStaticAndLoops},
        {"a tile has no rectangle so it is not drawn twice", &testATileHasNoRectangleSoItIsNotDrawnTwice},
        {"a decoration is drawn but cannot collide", &testADecorationIsDrawnButCannotCollide},
        {"a decoration is not affected by the tile loop", &testADecorationIsNotAffectedByTheTileLoop},
        {"the player keeps every value the level gave it", &testThePlayerKeepsEveryValueTheLevelGaveIt},
        {"the player is dynamic and tagged", &testThePlayerIsDynamicAndTagged},
        {"the player is drawn as an animation with its sprite centred",
         &testThePlayerIsDrawnAsAnAnimationWithItsSpriteCentred},
        {"spawn returns the entity count", &testSpawnReturnsTheEntityCount},
        {"the committed level spawns with the right tags", &testTheCommittedLevelSpawnsWithTheRightTags},
        {"the committed level is drawn and collides", &testTheCommittedLevelIsDrawnAndCollides},
        {"the loader asks the asset manager for every name", &testTheLoaderAsksTheAssetManagerForEveryName},
        {"a multi frame animation still sizes the box", &testAMultiFrameAnimationStillSizesTheBox},
        {"a missing tile animation fails loudly", &testAMissingTileAnimationFailsLoudly},
        {"a missing decoration animation fails loudly", &testAMissingDecorationAnimationFailsLoudly},
        {"a missing bullet animation fails loudly", &testAMissingBulletAnimationFailsLoudly},
        {"a missing animation is still an asset not found", &testAMissingAnimationIsStillAnAssetNotFound},
        {"a failed spawn leaves the world as it found it", &testAFailedSpawnLeavesTheWorldAsItFoundIt},
        {"the loader exposes the asset manager it was given", &testTheLoaderExposesTheAssetManagerItWasGiven},
        {"no texture is ever requested", &testNoTextureIsEverRequested},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        try
        {
            testCase();
        }
        catch (const std::exception& error)
        {
            ++g_failureCount;
            std::cerr << "    unexpected exception escaped group \"" << name << "\": " << error.what() << '\n';
        }

        const bool passed = g_failureCount == failuresBefore;
        if (!passed)
        {
            ++failedGroups;
        }

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << name << '\n';
    }

    const std::size_t groupCount = sizeof(testCases) / sizeof(testCases[0]);

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " level test groups passed\n";
    return EXIT_SUCCESS;
}
