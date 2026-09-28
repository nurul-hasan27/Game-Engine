#include "engine/assets/AssetFile.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
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

void checkEqual(const std::string& actual, const std::string& expected, const char* const expression,
                const char* const file, const int line)
{
    if (actual != expected)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual   = \"" << actual << "\"\n      expected = \"" << expected << "\"\n";
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_STR(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

using engine::assets::AssetEntry;
using engine::assets::AssetParseError;
using engine::assets::AssetType;
using engine::assets::parseAssetFile;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Parses `contents` and returns the `what()` of the `AssetParseError` it threw,
/// or an empty string if it did not throw.
///
/// Returning the message rather than a bool is deliberate. For a configuration
/// error the message *is* the user interface, so asserting on it exactly is
/// asserting on the contract, not on an implementation detail.
[[nodiscard]] std::string errorFrom(const std::string_view contents)
{
    try
    {
        (void)parseAssetFile(contents);
    }
    catch (const AssetParseError& error)
    {
        return error.what();
    }

    return {};
}

/// True when the message starts with `line <n>:`, which is the located-error
/// format every parse error is documented to use.
[[nodiscard]] bool reportsLine(const std::string& message, const std::size_t lineNumber)
{
    return message.rfind("line " + std::to_string(lineNumber) + ":", 0) == 0;
}

/// Reads one of the parser's own source files, so the suite can assert on the
/// isolation rules that cannot be observed from behaviour alone.
///
/// These paths come from the build system, so the test does not assume where the
/// repository is checked out.
[[nodiscard]] std::string readSource(const char* const path)
{
    std::ifstream file{path};
    if (!file)
    {
        std::cerr << "    unable to read source file: " << path << '\n';
        ++g_failureCount;
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool containsAny(const std::string& haystack, const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles)
    {
        if (haystack.find(needle) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Compile-time guarantees.
//
// The parser is plain data in, plain data out. Nothing about AssetEntry should
// ever acquire behaviour, and AssetType is a closed two-value set.
// ---------------------------------------------------------------------------

static_assert(std::is_aggregate_v<AssetEntry>, "AssetEntry must remain an aggregate");
static_assert(std::is_enum_v<AssetType>, "AssetType must be an enum");
static_assert(std::is_same_v<std::underlying_type_t<AssetType>, int>, "AssetType must have an int underlying type");
static_assert(static_cast<int>(AssetType::Texture) != static_cast<int>(AssetType::Font),
              "AssetType values must be distinct");
static_assert(std::is_default_constructible_v<AssetEntry>, "AssetEntry must be default constructible");

// The documented catch contract: a caller can handle a bad asset file as an
// invalid argument, or handle it specifically. Both have to keep working, so
// the relationship is asserted rather than described.
static_assert(std::is_base_of_v<std::invalid_argument, AssetParseError>,
              "AssetParseError must be catchable as std::invalid_argument");
static_assert(std::is_base_of_v<std::exception, AssetParseError>, "AssetParseError must be an exception");
static_assert(std::is_constructible_v<AssetParseError, std::string>,
              "AssetParseError must be constructible from the message it reports");

// ---------------------------------------------------------------------------
// Empty and ignorable input
// ---------------------------------------------------------------------------

void testEmptyInput()
{
    const std::vector<AssetEntry> entries = parseAssetFile("");

    CHECK(entries.empty());
}

void testBlankLinesOnly()
{
    const std::vector<AssetEntry> entries = parseAssetFile("\n\n   \n\t\n\r\n\n");

    CHECK(entries.empty());
}

void testCommentsAreIgnored()
{
    const std::vector<AssetEntry> entries = parseAssetFile("# a leading comment\n"
                                                          "\n"
                                                          "   # an indented comment\n"
                                                          "### a louder comment\n"
                                                          "#\n"
                                                          "#Texture mario mario.png\n"
                                                          "\n"
                                                          "# trailing comment\n");

    CHECK(entries.empty());
}

void testWhitespaceIsNotPartOfTokens()
{
    // Deliberately mixed tabs, spaces, leading and trailing padding. A
    // whitespace separated format has to keep its padding out of the tokens or
    // every name and path inherits stray spaces.
    const std::vector<AssetEntry> entries = parseAssetFile("\t  Texture \t mario \t mario.png  \t\n"
                                                          "   Font\tpixel\tpixeled.ttf   \n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[0].type == AssetType::Texture);
    CHECK_STR(entries[0].name, "mario");
    CHECK_STR(entries[0].path, "mario.png");
    CHECK(entries[1].type == AssetType::Font);
    CHECK_STR(entries[1].name, "pixel");
    CHECK_STR(entries[1].path, "pixeled.ttf");
}

// ---------------------------------------------------------------------------
// Well formed input
// ---------------------------------------------------------------------------

void testSingleTextureEntry()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario assets/library/images/mario/mario.png\n");

    CHECK(entries.size() == 1U);
    if (entries.size() != 1U)
    {
        return;
    }

    CHECK(entries[0].type == AssetType::Texture);
}

void testSingleFontEntry()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Font mario_font assets/library/fonts/mario.ttf\n");

    CHECK(entries.size() == 1U);
    if (entries.size() != 1U)
    {
        return;
    }

    CHECK(entries[0].type == AssetType::Font);
}

void testMultipleEntries()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture a a.png\n"
                                                          "Font b b.ttf\n"
                                                          "Texture c c.png\n"
                                                          "Font d d.ttf\n");

    CHECK(entries.size() == 4U);
}

void testFileOrderIsPreserved()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture zebra zebra.png\n"
                                                          "Font alpha alpha.ttf\n"
                                                          "Texture middle middle.png\n");

    CHECK(entries.size() == 3U);
    if (entries.size() != 3U)
    {
        return;
    }

    // Deliberately not alphabetical, so a parser that sorted would fail here.
    CHECK_STR(entries[0].name, "zebra");
    CHECK_STR(entries[1].name, "alpha");
    CHECK_STR(entries[2].name, "middle");
}

void testNamesAndPathsArePreservedExactly()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture player_stand library/images/megaman/megaStand.png\n"
                                                          "Font debug library/fonts/pixeled.ttf\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    // Punctuation, digits, underscores and separators all have to survive
    // untouched: these strings become file paths and lookup keys.
    CHECK_STR(entries[0].name, "player_stand");
    CHECK_STR(entries[0].path, "library/images/megaman/megaStand.png");
    CHECK_STR(entries[1].name, "debug");
    CHECK_STR(entries[1].path, "library/fonts/pixeled.ttf");
}

void testEntriesAreIndependentOfPriorCalls()
{
    // A first, different parse in between must not change the result. This is
    // the observable form of "no global state": if the parser accumulated
    // anything, or rejected a name it had seen before, this would differ.
    const std::string contents = "Texture mario mario.png\nFont debug debug.ttf\n";
    const std::string other = "Texture goomba goomba.png\n";

    const std::vector<AssetEntry> first = parseAssetFile(contents);
    (void)parseAssetFile(other);
    const std::vector<AssetEntry> third = parseAssetFile(contents);

    CHECK(first.size() == 2U);
    CHECK(third.size() == first.size());
    if (first.size() != 2U || third.size() != 2U)
    {
        return;
    }

    CHECK_STR(third[0].name, first[0].name);
    CHECK_STR(third[0].path, first[0].path);
    CHECK_STR(third[1].name, first[1].name);
    CHECK_STR(third[1].path, first[1].path);
}

void testWindowsLineEndingsParseTheSame()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario mario.png\r\nFont debug debug.ttf\r\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    // A trailing '\r' left on the path would make the file unopenable, and it
    // is the classic way a config written on one machine breaks on another.
    CHECK_STR(entries[0].name, "mario");
    CHECK_STR(entries[0].path, "mario.png");
    CHECK_STR(entries[1].name, "debug");
    CHECK_STR(entries[1].path, "debug.ttf");
}

// ---------------------------------------------------------------------------
// Rejection
// ---------------------------------------------------------------------------

void testUnknownAssetTypeIsRejected()
{
    CHECK_STR(errorFrom("Sound beep sounds/beep.wav\n"), "line 1: unknown asset type 'Sound'");
    CHECK_STR(errorFrom("texture mario mario.png\n"), "line 1: unknown asset type 'texture'");
    CHECK_STR(errorFrom("Sprite thing thing.png\n"), "line 1: unknown asset type 'Sprite'");
}

// ---------------------------------------------------------------------------
// Animations
//
// These replaced the two groups that used to assert `Animation` was rejected as
// unsupported. The keyword is now honoured, so those groups asserted a contract
// the engine has deliberately stopped having; keeping them would have meant a
// green suite lying about what the parser does.
// ---------------------------------------------------------------------------

void testAnimationEntryParsesEveryField()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run library/images/megaman/megaRun.png\n"
                                                          "Animation mario_running mario_run 3 5\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    const AssetEntry& animation = entries[1];
    CHECK(animation.type == AssetType::Animation);
    CHECK_STR(animation.name, "mario_running");
    CHECK_STR(animation.textureName, "mario_run");
    CHECK(animation.frameCount == 3U);
    CHECK(animation.speed == 5U);
}

void testAnimationEntryHasNoPath()
{
    // An animation names a texture, not a file, so there is nothing for `path` to
    // hold. It must be empty rather than holding the texture's path, or a loader
    // that walked every entry expecting a file would try to open it.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\n"
                                                          "Animation mario_running mario_run 3 5\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK_STR(entries[1].path, "");
}

void testTextureAndFontEntriesCarryNoAnimationFields()
{
    // The mirror of the previous group: the extra fields default rather than
    // picking up whatever happened to be on the line.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\n"
                                                          "Font debug debug.ttf\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK_STR(entries[0].textureName, "");
    CHECK(entries[0].frameCount == 0U);
    CHECK(entries[0].speed == 0U);
    CHECK_STR(entries[1].textureName, "");
    CHECK(entries[1].frameCount == 0U);
    CHECK(entries[1].speed == 0U);
}

void testAnimationMustFollowItsTexture()
{
    // The course's dependency rule, stated directly: a texture used by an
    // animation must already have been defined before the animation is loaded.
    const std::string message = errorFrom("Animation mario_running mario_run 3 5\n"
                                          "Texture mario_run library/images/megaman/megaRun.png\n");

    CHECK_STR(message, "line 1: Animation entry 'mario_running' animates texture 'mario_run', which no earlier "
                       "Texture entry declares; a texture must be defined before an animation that uses it");
}

void testAnimationWithUnknownTextureIsRejected()
{
    // Not merely "declared later" but never declared at all. The message must
    // quote the name that could not be found, because that is the thing to fix.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_walking mario_jump 2 4\n");

    CHECK_STR(message, "line 2: Animation entry 'mario_walking' animates texture 'mario_jump', which no earlier "
                       "Texture entry declares; a texture must be defined before an animation that uses it");
}

void testAnimationCannotAnimateAFont()
{
    // A font is not a texture. Treating any earlier entry as a valid source would
    // let a chain of animations reference each other with no image in it.
    const std::string message = errorFrom("Font debug debug.ttf\n"
                                          "Animation wobble debug 2 4\n");

    CHECK_STR(message, "line 2: Animation entry 'wobble' animates texture 'debug', which no earlier Texture entry "
                       "declares; a texture must be defined before an animation that uses it");
}

void testAnimationCannotAnimateAnotherAnimation()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation first mario_run 2 4\n"
                                          "Animation second first 2 4\n");

    CHECK_STR(message, "line 3: Animation entry 'second' animates texture 'first', which no earlier Texture entry "
                       "declares; a texture must be defined before an animation that uses it");
}

void testAnimationWithTwoTexturesPicksTheEarlierOne()
{
    // The rule is "declared before", not "declared most recently", and a second
    // declaration of the same name is impossible anyway. What this pins down is
    // that the check scans the whole prefix rather than only the previous line.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\n"
                                                          "Texture mario_jump jump.png\n"
                                                          "Font debug debug.ttf\n"
                                                          "\n"
                                                          "# a comment in between changes nothing\n"
                                                          "Animation mario_running mario_run 3 5\n");

    CHECK(entries.size() == 4U);
    if (entries.size() != 4U)
    {
        return;
    }

    CHECK(entries[3].type == AssetType::Animation);
    CHECK_STR(entries[3].textureName, "mario_run");
}

void testAnimationMissingNameIsRejected()
{
    // A bare keyword is one token where five are required, and the message names
    // the field that is absent rather than counting tokens, so it says what to
    // type. It quotes the animation's own syntax, not the texture's.
    CHECK_STR(errorFrom("Animation\n"), "line 1: Animation entry is missing a name; expected 'Animation <name> "
                                        "<textureName> <frameCount> <speed>'");

    // A keyword and nothing else, on a later line, reports that later line.
    CHECK_STR(errorFrom("Texture mario_run run.png\n"
                        "Animation\n"),
              "line 2: Animation entry is missing a name; expected 'Animation <name> <textureName> <frameCount> "
              "<speed>'");
}

void testAnimationMissingTextureNameIsRejected()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running\n");

    CHECK_STR(message, "line 2: Animation entry 'mario_running' is missing a textureName; expected 'Animation "
                       "<name> <textureName> <frameCount> <speed>'");
}

void testAnimationMissingFrameCountIsRejected()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run\n");

    CHECK_STR(message, "line 2: Animation entry 'mario_running' animating texture 'mario_run' is missing a "
                       "frameCount; expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationMissingSpeedIsRejected()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3\n");

    CHECK_STR(message, "line 2: Animation entry 'mario_running' animating texture 'mario_run' is missing a speed; "
                       "expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationWithTooManyTokensIsRejected()
{
    // The exact count is reported, not just "too many". A file author who wrote
    // six tokens needs to be told there are six.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3 5 extra\n");

    CHECK_STR(message, "line 2: Animation entry has 6 tokens; expected 'Animation <name> <textureName> <frameCount> "
                       "<speed>'");
}

void testAnimationRejectsZeroFrameCount()
{
    // Zero frames would divide a texture into nothing. Rejecting it here means the
    // loader can never be handed a divisor of zero.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 0 5\n");

    CHECK_STR(message, "line 2: Animation entry has frameCount '0', which is not a positive number this engine can "
                       "use; expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsZeroSpeed()
{
    // Zero speed is the reference implementation's divide-by-zero. Rejecting it
    // at parse time is the whole reason this cannot happen.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3 0\n");

    CHECK_STR(message, "line 2: Animation entry has speed '0', which is not a positive number this engine can use; "
                       "expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsNonNumericFrameCount()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run three 5\n");

    CHECK_STR(message, "line 2: Animation entry has frameCount 'three', which is not a positive decimal number; "
                       "expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsNonNumericSpeed()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3 fast\n");

    CHECK_STR(message, "line 2: Animation entry has speed 'fast', which is not a positive decimal number; expected "
                       "'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsPartiallyNumericField()
{
    // `std::stoi("3x")` returns 3 and ignores the `x`. This must not, or a typo
    // in a frame count would load as a different, valid animation.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3x 5\n");

    CHECK_STR(message, "line 2: Animation entry has frameCount '3x', which is not a positive decimal number; "
                       "expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsNegativeNumbers()
{
    // A leading `-` is not a digit, so this is a shape error rather than a range
    // one. What matters is that it is refused.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3 -5\n");

    CHECK_STR(message, "line 2: Animation entry has speed '-5', which is not a positive decimal number; expected "
                       "'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsOversizedFrameCount()
{
    // Ten digits is the most that can be examined without risking overflow, and
    // 9999999999 does not fit in the 32-bit field. It must be refused with a
    // message about the value, not by wrapping to something small.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 99999999999 5\n");

    CHECK_STR(message, "line 2: Animation entry has frameCount '99999999999', which is not a positive decimal "
                       "number; expected 'Animation <name> <textureName> <frameCount> <speed>'");
}

void testAnimationRejectsFrameCountBeyondThirtyTwoBits()
{
    // 4294967295 does fit in uint32_t, so this is accepted by the field check and
    // is the loader's problem to reject against a real image width. The point of
    // this group is that it is *not* refused here, which is what makes the
    // loader's divisibility check the only place it can be caught.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\n"
                                                          "Animation mario_running mario_run 4294967295 5\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[1].frameCount == 4294967295U);
}

void testAnimationNameIsUniqueAgainstATexture()
{
    const std::string message = errorFrom("Texture shared shared.png\n"
                                          "Animation shared mario_run 2 4\n");

    CHECK_STR(message, "line 2: duplicate asset name 'shared'; it is already declared as a Texture");
}

void testAnimationNameIsUniqueAgainstAFont()
{
    const std::string message = errorFrom("Font shared shared.ttf\n"
                                          "Texture mario_run run.png\n"
                                          "Animation shared mario_run 2 4\n");

    CHECK_STR(message, "line 3: duplicate asset name 'shared'; it is already declared as a Font");
}

void testAnimationNameIsUniqueAgainstAnotherAnimation()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation twin mario_run 2 4\n"
                                          "Animation twin mario_run 3 4\n");

    CHECK_STR(message, "line 3: duplicate asset name 'twin'; it is already declared as a Animation");
}

void testTextureNameIsUniqueAgainstAnAnimation()
{
    // The same flat namespace seen from the other direction: the animation claims
    // the name first and the texture that follows is the one refused. The names
    // are distinct from the texture the animation slices, so the only collision
    // is the one under test.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_dashing mario_run 2 4\n"
                                          "Texture mario_dashing other.png\n");

    CHECK_STR(message, "line 3: duplicate asset name 'mario_dashing'; it is already declared as a Animation");
}

void testAnimationErrorNamesTheLineNumber()
{
    // Comments and blanks count toward the number, exactly as they do for a
    // texture, so the reported line is the real one.
    const std::string message = errorFrom("# header\n"
                                          "\n"
                                          "Texture mario_run run.png\n"
                                          "# a comment\n"
                                          "\n"
                                          "Animation mario_running nowhere 3 5\n");

    CHECK(reportsLine(message, 6U));
}

void testAnimationErrorIsReportedBeforeTheTextureIsChecked()
{
    // A line wrong in two ways reports the shape problem, because the numbers
    // have to be readable before the reference means anything.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running nowhere zero 5\n");

    CHECK(message.find("frameCount") != std::string::npos);
    CHECK(message.find("nowhere") == std::string::npos);
}

void testAnimationErrorIsReportedBeforeADuplicateName()
{
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run zero 5\n"
                                          "Animation mario_running mario_run 2 4\n");

    CHECK(message.find("frameCount") != std::string::npos);
}

void testMalformedAnimationStopsParsingAtThatLine()
{
    // Nothing after a bad animation may be returned, so a caller cannot load half
    // a file. The group also proves the *animation* is what stopped it, by making
    // the lines after it the only thing that would otherwise have parsed.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running nowhere 3 5\n"
                                          "Texture mario_jump jump.png\n"
                                          "Font debug debug.ttf\n");

    CHECK_STR(message, "line 2: Animation entry 'mario_running' animates texture 'nowhere', which no earlier "
                       "Texture entry declares; a texture must be defined before an animation that uses it");
}

void testTrailingCommentIsStillRejectedOnAnAnimationLine()
{
    // Whole-line comments only, for every entry kind. A trailing `#` on an
    // animation must not become a fifth-and-a-half field.
    // The exact count is reported, and it counts the `#` and the words after it:
    // the animation line is not being given a special exemption from the rule
    // that stops a trailing comment being mistaken for a field.
    const std::string message = errorFrom("Texture mario_run run.png\n"
                                          "Animation mario_running mario_run 3 5 # walk cycle\n");

    CHECK_STR(message, "line 2: Animation entry has 8 tokens; expected 'Animation <name> <textureName> <frameCount> "
                       "<speed>'");
}

void testCommentsAndBlanksAroundAnimationsAreIgnored()
{
    const std::vector<AssetEntry> entries = parseAssetFile("# animations below\n"
                                                          "\n"
                                                          "Texture mario_run run.png\n"
                                                          "   \n"
                                                          "#Animation not_a_real_entry run.png\n"
                                                          "Animation mario_running mario_run 3 5\n"
                                                          "\n"
                                                          "# trailing comment\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[1].type == AssetType::Animation);
}

void testAnimationsInterleaveWithEveryOtherKind()
{
    // A realistic file: textures, then animations that slice them, then more
    // textures, then more animations referencing those. The rule is per-entry,
    // not per-section, so an animation may follow any earlier texture.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture stand stand.png\n"
                                                          "Animation stand_idle stand 1 1\n"
                                                          "Texture run run.png\n"
                                                          "Animation run_cycle run 3 8\n"
                                                          "Font debug debug.ttf\n"
                                                          "Texture death death.png\n"
                                                          "Animation death_spin death 2 6\n");

    CHECK(entries.size() == 7U);
    if (entries.size() != 7U)
    {
        return;
    }

    CHECK(entries[1].type == AssetType::Animation);
    CHECK(entries[3].type == AssetType::Animation);
    CHECK(entries[4].type == AssetType::Font);
    CHECK(entries[5].type == AssetType::Texture);
    CHECK(entries[6].type == AssetType::Animation);
    CHECK_STR(entries[6].textureName, "death");
    CHECK(entries[6].frameCount == 2U);
    CHECK(entries[6].speed == 6U);
}

void testSeveralAnimationsShareOneTexture()
{
    // One texture, several animations over it. Sharing is the point: the loader
    // loads the image once and the animations are definitions over it.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture sheet sheet.png\n"
                                                          "Animation sheet_walk sheet 4 6\n"
                                                          "Animation sheet_run  sheet 4 3\n"
                                                          "Animation sheet_once sheet 8 2\n");

    CHECK(entries.size() == 4U);
    if (entries.size() != 4U)
    {
        return;
    }

    CHECK_STR(entries[1].textureName, "sheet");
    CHECK_STR(entries[2].textureName, "sheet");
    CHECK_STR(entries[3].textureName, "sheet");
    CHECK(entries[3].frameCount == 8U);
}

void testSingleFrameAnimationIsAccepted()
{
    // The course's level format makes every entity name an animation, including
    // ones that never change, so a one-frame animation is normal rather than
    // degenerate. The reference configuration declares thirteen of them.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture brick brick.png\n"
                                                          "Animation brick_still brick 1 1\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[1].frameCount == 1U);
    CHECK(entries[1].speed == 1U);
}

void testAnimationDoesNotAffectTextureOrFontParsing()
{
    // The same texture and font lines that parsed before animation support
    // existed must still parse, byte for byte, now that it does. This is the
    // regression guard for the shared tokenizer being resized from three fields
    // to five.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario mario.png\n"
                                                          "Font debug debug.ttf\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[0].type == AssetType::Texture);
    CHECK_STR(entries[0].name, "mario");
    CHECK_STR(entries[0].path, "mario.png");
    CHECK(entries[1].type == AssetType::Font);
    CHECK_STR(entries[1].name, "debug");
    CHECK_STR(entries[1].path, "debug.ttf");
}

void testWindowsLineEndingsParseAnimationsToo()
{
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\r\n"
                                                          "Animation mario_running mario_run 3 5\r\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[1].frameCount == 3U);
    CHECK(entries[1].speed == 5U);
    CHECK_STR(entries[1].textureName, "mario_run");
}

void testLeadingZeroesInNumbersAreAccepted()
{
    // Padded columns are a normal way to align a configuration file, and they
    // still name the same numbers. Rejecting them would be gratuitous.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario_run run.png\n"
                                                          "Animation mario_running mario_run 003 005\n");

    CHECK(entries.size() == 2U);
    if (entries.size() != 2U)
    {
        return;
    }

    CHECK(entries[1].frameCount == 3U);
    CHECK(entries[1].speed == 5U);
}

void testAnimationIsRejectedWhenItsTextureIsDeclaredAfterAFontOfTheSameName()
{
    // The name is what is matched, and only against Texture entries. A font that
    // happens to share the name must not satisfy the reference.
    const std::string message = errorFrom("Font shared shared.ttf\n"
                                          "Animation wobble shared 2 4\n"
                                          "Texture shared shared.png\n");

    CHECK_STR(message, "line 2: Animation entry 'wobble' animates texture 'shared', which no earlier Texture entry "
                       "declares; a texture must be defined before an animation that uses it");
}

void testMissingNameIsRejected()
{
    CHECK_STR(errorFrom("Texture\n"), "line 1: Texture entry is missing a name; expected 'Texture <name> <path>'");
    CHECK_STR(errorFrom("Font\n"), "line 1: Font entry is missing a name; expected 'Texture <name> <path>'");
}

void testBlankNameIsRejected()
{
    // A blank field and an absent field are indistinguishable in a whitespace
    // separated format, so both spellings have to be rejected, with the same
    // message. Trailing whitespace must not turn a broken line into a valid one.
    CHECK_STR(errorFrom("Texture   \n"), "line 1: Texture entry is missing a name; expected 'Texture <name> <path>'");
    CHECK_STR(errorFrom("Texture \t \t\n"), "line 1: Texture entry is missing a name; expected 'Texture <name> <path>'");
}

void testMissingPathIsRejected()
{
    CHECK_STR(errorFrom("Texture mario\n"),
              "line 1: Texture entry 'mario' is missing a path; expected 'Texture <name> <path>'");
    CHECK_STR(errorFrom("Font debug\n"),
              "line 1: Font entry 'debug' is missing a path; expected 'Texture <name> <path>'");
}

void testBlankPathIsRejected()
{
    CHECK_STR(errorFrom("Texture mario   \n"),
              "line 1: Texture entry 'mario' is missing a path; expected 'Texture <name> <path>'");
}

void testTooManyTokensIsRejected()
{
    CHECK_STR(errorFrom("Texture mario mario.png extra\n"),
              "line 1: Texture entry has 4 tokens; expected 'Texture <name> <path>'");

    // The count in the message is the real one, not however many the parser
    // happened to keep. A user told "4 tokens" about a six-token line would go
    // and delete the wrong field.
    CHECK_STR(errorFrom("Font debug debug.ttf a b c\n"),
              "line 1: Font entry has 6 tokens; expected 'Texture <name> <path>'");
    CHECK_STR(errorFrom("Texture a b c d e f g h\n"),
              "line 1: Texture entry has 9 tokens; expected 'Texture <name> <path>'");
}

void testTrailingCommentIsNotAccepted()
{
    // Only whole lines may be commented. Accepting a trailing '#' would mean a
    // path containing '#' changes meaning depending on position, which is
    // exactly the kind of quiet reinterpretation this format refuses.
    CHECK_STR(errorFrom("Texture mario mario.png # the player\n"),
              "line 1: Texture entry has 6 tokens; expected 'Texture <name> <path>'");
}

void testDuplicateNameIsRejected()
{
    CHECK_STR(errorFrom("Texture mario a.png\nTexture mario b.png\n"),
              "line 2: duplicate asset name 'mario'; it is already declared as a Texture");
}

void testDuplicateNameAcrossTypesIsRejected()
{
    // One flat namespace, because a component will hold a bare name and so a
    // shared name would stop identifying exactly one resource.
    CHECK_STR(errorFrom("Font mario a.ttf\nTexture mario b.png\n"),
              "line 2: duplicate asset name 'mario'; it is already declared as a Font");
    CHECK_STR(errorFrom("Texture mario b.png\nFont mario a.ttf\n"),
              "line 2: duplicate asset name 'mario'; it is already declared as a Texture");
}

void testDuplicateIsNotConfusedBySimilarNames()
{
    // Only exact matches are duplicates. A prefix, a suffix or a case difference
    // is a different name, and rejecting those would be a real usability bug.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture mario mario.png\n"
                                                          "Texture mario2 mario2.png\n"
                                                          "Texture Mario mario.png\n"
                                                          "Texture mari mario.png\n");

    CHECK(entries.size() == 4U);
}

void testErrorReportsTheCorrectLineNumber()
{
    const std::string message = errorFrom("Texture a a.png\n"
                                          "Font b b.ttf\n"
                                          "Texture c c.png\n"
                                          "Font d d.ttf\n"
                                          "Texture e e.png\n"
                                          "Font\n");

    CHECK(reportsLine(message, 6U));
    CHECK_STR(message, "line 6: Font entry is missing a name; expected 'Texture <name> <path>'");
}

void testErrorLineNumberCountsCommentsAndBlanks()
{
    // The reported number is the physical line in the file, which is what a user
    // opening the file in an editor needs. Comments and blank lines are skipped,
    // but they are still lines.
    const std::string message = errorFrom("# one\n"
                                          "\n"
                                          "# three\n"
                                          "Texture mario mario.png\n"
                                          "\n"
                                          "# six\n"
                                          "Sound beep beep.wav\n");

    CHECK(reportsLine(message, 7U));
}

void testParsingStopsAtTheFirstError()
{
    // Nothing is returned alongside the error, so a caller cannot load half a
    // file. A good entry before the bad one must not escape.
    const std::string message = errorFrom("Texture a a.png\n"
                                          "Texture b b.png\n"
                                          "Sound beep beep.wav\n"
                                          "Texture c c.png\n");

    CHECK(reportsLine(message, 3U));
}

void testFundamentalTypeErrorBeatsTokenCount()
{
    // A line that is wrong in two ways should report the more fundamental
    // problem. Telling the user about token counts when the real issue is an
    // unrecognised keyword would send them editing whitespace for nothing. Note
    // the keyword here is genuinely unknown, not `Animation`: the arity of a
    // recognised type is a real question, but it is a question about a line the
    // engine understood.
    const std::string message = errorFrom("Sprite explosion a.png b.png c.png d.png e.png\n");

    CHECK(reportsLine(message, 1U));
    CHECK_STR(message, "line 1: unknown asset type 'Sprite'");
}

// ---------------------------------------------------------------------------
// A realistic configuration
// ---------------------------------------------------------------------------

void testRealisticMixedConfiguration()
{
    // Shaped like a real asset file: a commented header, a blank line, then a
    // mixed run of textures, animations that slice them, and fonts. The parser
    // only ever sees strings here, so this test needs no file on disk.
    const std::vector<AssetEntry> entries = parseAssetFile("# engine asset configuration\n"
                                                          "# paths are relative to this file\n"
                                                          "\n"
                                                          "Texture player_stand library/images/megaman/megaStand.png\n"
                                                          "Texture player_run   library/images/megaman/megaRun.png\n"
                                                          "Animation player_run_cycle player_run 3 8\n"
                                                          "Texture ground_tile  library/images/mario/ground.png\n"
                                                          "Animation ground_still ground_tile 1 1\n"
                                                          "Texture question     library/images/mario/question.png\n"
                                                          "Texture bullet       library/images/megaman/megaBuster.png\n"
                                                          "Animation bullet_spin bullet 2 6\n"
                                                          "\n"
                                                          "Font debug library/fonts/pixeled.ttf\n"
                                                          "Font hud library/fonts/tech.ttf\n"
                                                          "\n");

    CHECK(entries.size() == 10U);
    if (entries.size() != 10U)
    {
        return;
    }

    int textureCount = 0;
    int fontCount = 0;
    int animationCount = 0;
    for (const AssetEntry& entry : entries)
    {
        if (entry.type == AssetType::Texture)
        {
            ++textureCount;
            CHECK(!entry.path.empty());
        }
        else if (entry.type == AssetType::Font)
        {
            ++fontCount;
            CHECK(!entry.path.empty());
        }
        else
        {
            ++animationCount;
            // An animation has no path, only a reference and two numbers, so the
            // "every entry has a path" assumption this loop used to make is
            // exactly what animation support invalidated.
            CHECK_STR(entry.path, "");
            CHECK(!entry.textureName.empty());
            CHECK(entry.frameCount >= 1U);
            CHECK(entry.speed >= 1U);
        }

        CHECK(!entry.name.empty());
    }

    CHECK(textureCount == 5);
    CHECK(fontCount == 2);
    CHECK(animationCount == 3);

    // The aligned column layout must not leak into the names, and the first and
    // last real entries must be where a reader expects them.
    CHECK_STR(entries[0].name, "player_stand");
    CHECK_STR(entries[0].path, "library/images/megaman/megaStand.png");
    CHECK_STR(entries[6].name, "bullet");
    CHECK_STR(entries[6].path, "library/images/megaman/megaBuster.png");
    CHECK_STR(entries[8].name, "debug");
    CHECK_STR(entries[9].name, "hud");
}

// ---------------------------------------------------------------------------
// Isolation from the filesystem
// ---------------------------------------------------------------------------

void testParserPerformsNoFilesystemAccess()
{
    // Every path here is invented and does not exist anywhere. A parser that
    // opened, stat-ed or canonicalised any of them would fail this group, so
    // this is the behavioural half of the "no filesystem access" rule. The
    // names are all distinct because this configuration has to be valid input
    // to prove anything: it is being used to show the parser *accepts* it.
    const std::vector<AssetEntry> entries = parseAssetFile("Texture absent /definitely/not/here/missing.png\n"
                                                          "Font nonexistent /also/not/here/missing.ttf\n"
                                                          "Texture relative ../outside/the/tree.png\n"
                                                          "Texture dotted ./././././here.png\n");

    CHECK(entries.size() == 4U);
    if (entries.size() != 4U)
    {
        return;
    }

    // Stored exactly as written, with no normalisation of any kind.
    CHECK_STR(entries[0].path, "/definitely/not/here/missing.png");
    CHECK_STR(entries[1].path, "/also/not/here/missing.ttf");
    CHECK_STR(entries[2].path, "../outside/the/tree.png");
    CHECK_STR(entries[3].path, "./././././here.png");
}

void testParserSourceHasNoSfmlReferences()
{
    const std::string header = readSource(ENGINE_ASSET_FILE_HEADER);
    const std::string source = readSource(ENGINE_ASSET_FILE_SOURCE);

    CHECK(!header.empty());
    CHECK(!source.empty());
    if (header.empty() || source.empty())
    {
        return;
    }

    // The parser is the one part of the engine that must stay usable with no
    // graphics stack at all. Its own source is where that would be lost, so it
    // is checked rather than trusted.
    //
    // The needles are deliberately the *ways SFML is actually reached* in C++,
    // not the bare word. A prose mention of SFML in a documentation comment is
    // fine, and this file's own header says it uses none; only an include
    // directive or a qualified name can actually pull the library in. A bare
    // word search would report this very comment as a violation.
    //
    // The build backs this up at link time: asset_file_test links engine_assets
    // rather than engine, so SFML is not even on its link line.
    const std::vector<std::string> forbidden{"<SFML", "sf::"};

    CHECK_FALSE(containsAny(header, forbidden));
    CHECK_FALSE(containsAny(source, forbidden));
}

void testParserSourceHasNoFilesystemAccess()
{
    const std::string header = readSource(ENGINE_ASSET_FILE_HEADER);
    const std::string source = readSource(ENGINE_ASSET_FILE_SOURCE);

    CHECK(!header.empty());
    CHECK(!source.empty());
    if (header.empty() || source.empty())
    {
        return;
    }

    // Same reasoning as the SFML check: these are the specific spellings that
    // would open, read or stat something. Plain English words like "read" are
    // not needles, because a comment is allowed to use them.
    const std::vector<std::string> forbidden{"<fstream", "<filesystem", "<cstdio", "std::fstream", "std::ifstream",
                                             "std::ofstream", "std::filesystem", "::stat(", "fopen(", "std::cin"};

    CHECK_FALSE(containsAny(header, forbidden));
    CHECK_FALSE(containsAny(source, forbidden));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"empty input", &testEmptyInput},
        {"blank lines only", &testBlankLinesOnly},
        {"comments are ignored", &testCommentsAreIgnored},
        {"whitespace is not part of tokens", &testWhitespaceIsNotPartOfTokens},
        {"single texture entry", &testSingleTextureEntry},
        {"single font entry", &testSingleFontEntry},
        {"multiple entries", &testMultipleEntries},
        {"file order is preserved", &testFileOrderIsPreserved},
        {"names and paths are preserved exactly", &testNamesAndPathsArePreservedExactly},
        {"entries are independent of prior calls", &testEntriesAreIndependentOfPriorCalls},
        {"windows line endings parse the same", &testWindowsLineEndingsParseTheSame},
        {"unknown asset type is rejected", &testUnknownAssetTypeIsRejected},
        {"animation entry parses every field", &testAnimationEntryParsesEveryField},
        {"animation entry has no path", &testAnimationEntryHasNoPath},
        {"texture and font entries carry no animation fields", &testTextureAndFontEntriesCarryNoAnimationFields},
        {"animation must follow its texture", &testAnimationMustFollowItsTexture},
        {"animation with unknown texture is rejected", &testAnimationWithUnknownTextureIsRejected},
        {"animation cannot animate a font", &testAnimationCannotAnimateAFont},
        {"animation cannot animate another animation", &testAnimationCannotAnimateAnotherAnimation},
        {"animation with two textures picks the earlier one", &testAnimationWithTwoTexturesPicksTheEarlierOne},
        {"animation missing name is rejected", &testAnimationMissingNameIsRejected},
        {"animation missing texture name is rejected", &testAnimationMissingTextureNameIsRejected},
        {"animation missing frame count is rejected", &testAnimationMissingFrameCountIsRejected},
        {"animation missing speed is rejected", &testAnimationMissingSpeedIsRejected},
        {"animation with too many tokens is rejected", &testAnimationWithTooManyTokensIsRejected},
        {"animation rejects zero frame count", &testAnimationRejectsZeroFrameCount},
        {"animation rejects zero speed", &testAnimationRejectsZeroSpeed},
        {"animation rejects non numeric frame count", &testAnimationRejectsNonNumericFrameCount},
        {"animation rejects non numeric speed", &testAnimationRejectsNonNumericSpeed},
        {"animation rejects partially numeric field", &testAnimationRejectsPartiallyNumericField},
        {"animation rejects negative numbers", &testAnimationRejectsNegativeNumbers},
        {"animation rejects oversized frame count", &testAnimationRejectsOversizedFrameCount},
        {"animation accepts frame count beyond thirty two bits", &testAnimationRejectsFrameCountBeyondThirtyTwoBits},
        {"animation name is unique against a texture", &testAnimationNameIsUniqueAgainstATexture},
        {"animation name is unique against a font", &testAnimationNameIsUniqueAgainstAFont},
        {"animation name is unique against another animation", &testAnimationNameIsUniqueAgainstAnotherAnimation},
        {"texture name is unique against an animation", &testTextureNameIsUniqueAgainstAnAnimation},
        {"animation error names the line number", &testAnimationErrorNamesTheLineNumber},
        {"animation error beats the texture check", &testAnimationErrorIsReportedBeforeTheTextureIsChecked},
        {"animation error beats a duplicate name", &testAnimationErrorIsReportedBeforeADuplicateName},
        {"malformed animation stops parsing at that line", &testMalformedAnimationStopsParsingAtThatLine},
        {"trailing comment is still rejected on an animation line", &testTrailingCommentIsStillRejectedOnAnAnimationLine},
        {"comments and blanks around animations are ignored", &testCommentsAndBlanksAroundAnimationsAreIgnored},
        {"animations interleave with every other kind", &testAnimationsInterleaveWithEveryOtherKind},
        {"several animations share one texture", &testSeveralAnimationsShareOneTexture},
        {"single frame animation is accepted", &testSingleFrameAnimationIsAccepted},
        {"animation does not affect texture or font parsing", &testAnimationDoesNotAffectTextureOrFontParsing},
        {"windows line endings parse animations too", &testWindowsLineEndingsParseAnimationsToo},
        {"leading zeroes in numbers are accepted", &testLeadingZeroesInNumbersAreAccepted},
        {"animation is rejected when a font shadows the texture name", &testAnimationIsRejectedWhenItsTextureIsDeclaredAfterAFontOfTheSameName},
        {"missing name is rejected", &testMissingNameIsRejected},
        {"blank name is rejected", &testBlankNameIsRejected},
        {"missing path is rejected", &testMissingPathIsRejected},
        {"blank path is rejected", &testBlankPathIsRejected},
        {"too many tokens is rejected", &testTooManyTokensIsRejected},
        {"trailing comment is not accepted", &testTrailingCommentIsNotAccepted},
        {"duplicate name is rejected", &testDuplicateNameIsRejected},
        {"duplicate name across types is rejected", &testDuplicateNameAcrossTypesIsRejected},
        {"duplicate is not confused by similar names", &testDuplicateIsNotConfusedBySimilarNames},
        {"error reports the correct line number", &testErrorReportsTheCorrectLineNumber},
        {"error line number counts comments and blanks", &testErrorLineNumberCountsCommentsAndBlanks},
        {"parsing stops at the first error", &testParsingStopsAtTheFirstError},
        {"fundamental type error beats token count", &testFundamentalTypeErrorBeatsTokenCount},
        {"realistic mixed configuration", &testRealisticMixedConfiguration},
        {"parser performs no filesystem access", &testParserPerformsNoFilesystemAccess},
        {"parser source has no sfml references", &testParserSourceHasNoSfmlReferences},
        {"parser source has no filesystem access", &testParserSourceHasNoFilesystemAccess},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        // Every function under test is documented to throw, so an exception
        // escaping a group is a failure of that group, not a reason to abort the
        // binary. Letting it propagate would kill the run and hide the result of
        // every group after this one, which is exactly when the information is
        // most wanted.
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

    const auto groupCount = sizeof(testCases) / sizeof(testCases[0]);

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " asset configuration parser test groups passed\n";
    return EXIT_SUCCESS;
}
