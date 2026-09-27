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

void testAnimationIsRejectedAsUnsupported()
{
    const std::string message =
        errorFrom("Animation explosion assets/library/images/animations/explosion.png\n");

    // The message has to distinguish "not supported yet" from "I have never
    // heard of this keyword", because Animation really is a keyword in the
    // course asset format and a user will reasonably have copied it.
    CHECK_STR(message, "line 1: 'Animation' is not supported yet; this engine understands only 'Texture' and 'Font' "
                        "entries");
    CHECK(reportsLine(message, 1U));
}

void testAnimationErrorNamesTheLineNumber()
{
    const std::string message = errorFrom("# header\n"
                                          "\n"
                                          "Texture mario mario.png\n"
                                          "Animation explosion explosion.png\n");

    CHECK(reportsLine(message, 4U));
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
    // unsupported keyword would send them editing whitespace for nothing.
    const std::string message = errorFrom("Animation explosion a.png b.png\n");

    CHECK(reportsLine(message, 1U));
    CHECK(message.find("not supported yet") != std::string::npos);
}

// ---------------------------------------------------------------------------
// A realistic configuration
// ---------------------------------------------------------------------------

void testRealisticMixedConfiguration()
{
    // Shaped like a real asset file: a commented header, a blank line, a mixed
    // run of textures and fonts, then a comment. The parser only ever sees
    // strings here, so this test needs no file on disk.
    const std::vector<AssetEntry> entries = parseAssetFile("# engine asset configuration\n"
                                                          "# paths are relative to this file\n"
                                                          "\n"
                                                          "Texture player_stand library/images/megaman/megaStand.png\n"
                                                          "Texture player_run   library/images/megaman/megaRun.png\n"
                                                          "Texture ground_tile  library/images/mario/ground.png\n"
                                                          "Texture question     library/images/mario/question.png\n"
                                                          "Texture bullet       library/images/megaman/megaBuster.png\n"
                                                          "\n"
                                                          "Font debug library/fonts/pixeled.ttf\n"
                                                          "Font hud library/fonts/tech.ttf\n"
                                                          "\n"
                                                          "# animations arrive with the animation phase\n");

    CHECK(entries.size() == 7U);
    if (entries.size() != 7U)
    {
        return;
    }

    int textureCount = 0;
    int fontCount = 0;
    for (const AssetEntry& entry : entries)
    {
        if (entry.type == AssetType::Texture)
        {
            ++textureCount;
        }
        else
        {
            ++fontCount;
        }

        CHECK(!entry.name.empty());
        CHECK(!entry.path.empty());
    }

    CHECK(textureCount == 5);
    CHECK(fontCount == 2);

    // The aligned column layout must not leak into the names, and the first and
    // last real entries must be where a reader expects them.
    CHECK_STR(entries[0].name, "player_stand");
    CHECK_STR(entries[0].path, "library/images/megaman/megaStand.png");
    CHECK_STR(entries[4].name, "bullet");
    CHECK_STR(entries[4].path, "library/images/megaman/megaBuster.png");
    CHECK_STR(entries[5].name, "debug");
    CHECK_STR(entries[6].name, "hud");
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
        {"animation is rejected as unsupported", &testAnimationIsRejectedAsUnsupported},
        {"animation error names the line number", &testAnimationErrorNamesTheLineNumber},
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
