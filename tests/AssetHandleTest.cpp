#include "engine/assets/Font.hpp"
#include "engine/assets/Texture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
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

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)

using engine::assets::Font;
using engine::assets::Texture;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Reads one of the asset types' own source files, so the suite can assert on the
/// boundary rules that cannot be observed from behaviour alone: a header that
/// quietly pulled in SFML would still compile and still pass every runtime test.
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

[[nodiscard]] bool containsAll(const std::string& haystack, const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles)
    {
        if (haystack.find(needle) == std::string::npos)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::string::size_type at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
    {
        ++count;
    }

    return count;
}

/// Removes `//` comments and blank lines, leaving only code.
///
/// Both asset headers discuss SFML in their documentation, and the very sentence
/// saying "this header mentions no `sf::` type" contains the characters a
/// substring search is looking for. Scanning the raw text therefore reports these
/// headers as violations of the rule they were written to state. The rule is
/// about code, so the comments go first and the search runs on what is left.
///
/// Block comments are not handled, and neither header uses one; a line whose
/// only content is `/*` would survive as a stray token rather than being
/// misread as code.
[[nodiscard]] std::string stripComments(const std::string& source)
{
    std::string code;
    std::istringstream lines{source};
    std::string line;

    while (std::getline(lines, line))
    {
        // substr with npos yields the whole string, which is the no-comment case.
        const std::string withoutComment = line.substr(0, line.find("//"));

        if (withoutComment.find_first_not_of(" \t") != std::string::npos)
        {
            code += withoutComment;
            code += '\n';
        }
    }

    return code;
}

/// Everything the suite reads up front, already stripped of comments, so a test
/// can bail out rather than assert against a file it failed to open.
struct Sources
{
    std::string textureHeader;
    std::string fontHeader;
    std::string textureSource;
    std::string fontSource;
};

Sources loadSources()
{
    return Sources{stripComments(readSource(ENGINE_TEXTURE_HEADER)), stripComments(readSource(ENGINE_FONT_HEADER)),
                   stripComments(readSource(ENGINE_TEXTURE_SOURCE)), stripComments(readSource(ENGINE_FONT_SOURCE))};
}

// ---------------------------------------------------------------------------
// Compile-time guarantees: the special member contract.
//
// These are static_asserts rather than runtime checks on purpose. "Texture is
// not copyable" is a property that has to be true for every translation unit
// that includes the header, not merely true on the machine the test ran on, and
// only the compiler can guarantee that. A runtime test could only prove it by
// compiling a copy, which would then have to be deleted again.
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<Texture>, "Texture must not be copy constructible");
static_assert(!std::is_copy_assignable_v<Texture>, "Texture must not be copy assignable");
static_assert(!std::is_copy_constructible_v<Font>, "Font must not be copy constructible");
static_assert(!std::is_copy_assignable_v<Font>, "Font must not be copy assignable");

static_assert(std::is_move_constructible_v<Texture>, "Texture must be move constructible");
static_assert(std::is_move_assignable_v<Texture>, "Texture must be move assignable");
static_assert(std::is_move_constructible_v<Font>, "Font must be move constructible");
static_assert(std::is_move_assignable_v<Font>, "Font must be move assignable");

static_assert(std::is_default_constructible_v<Texture>, "Texture must be default constructible");
static_assert(std::is_default_constructible_v<Font>, "Font must be default constructible");
static_assert(std::is_destructible_v<Texture>, "Texture must be destructible");
static_assert(std::is_destructible_v<Font>, "Font must be destructible");

// A move must not be allowed to throw, or a vector reallocating its buffer
// would be forced to fall back to copying, which these types cannot do.
static_assert(std::is_nothrow_move_constructible_v<Texture>, "Texture move construction must be noexcept");
static_assert(std::is_nothrow_move_assignable_v<Texture>, "Texture move assignment must be noexcept");
static_assert(std::is_nothrow_move_constructible_v<Font>, "Font move construction must be noexcept");
static_assert(std::is_nothrow_move_assignable_v<Font>, "Font move assignment must be noexcept");

// The whole point of the opaque pointer. The header is not allowed to grow
// members of its own: if it ever holds a real resource rather than a pointer to
// one, this fails and the privacy has been broken.
static_assert(sizeof(Texture) == sizeof(void*), "Texture must be exactly one pointer wide");
static_assert(sizeof(Font) == sizeof(void*), "Font must be exactly one pointer wide");
static_assert(std::is_standard_layout_v<Texture>, "Texture must remain standard layout");
static_assert(std::is_standard_layout_v<Font>, "Font must remain standard layout");

// A handle must be usable where a movable value is expected, which is the whole
// reason move support exists. std::vector reallocates by moving, so this is the
// requirement that would actually break if the moves were removed.
static_assert(std::is_move_constructible_v<std::vector<Texture>>, "Texture must work in a vector");
static_assert(std::is_move_constructible_v<std::vector<Font>>, "Font must work in a vector");

// A handle has no natural order, and must not be given one. Two loaded textures
// have no meaningful "less than", so an ordering would be an arbitrary
// implementation detail that a caller could then come to depend on. This is
// asserted so that somebody tidying the class later cannot add an operator< to
// make a sort compile, which is how accidental API tends to arrive.
static_assert(!std::is_invocable_v<std::less<>, Texture, Texture>, "Texture must not be orderable");
static_assert(!std::is_invocable_v<std::less<>, Font, Font>, "Font must not be orderable");

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void testTextureCanBeDefaultConstructed()
{
    const Texture texture;

    // Nothing to observe yet, and that is the point: an empty handle is a real
    // state, and it is reachable. Any size or predicate added later must not
    // make construction do real work.
    (void)texture;
}

void testFontCanBeDefaultConstructed()
{
    const Font font;

    (void)font;
}

void testTextureAndFontAreDistinctTypes()
{
    // The two handles exist side by side in the same namespace, so they must not
    // be the same type wearing two names. Without an SFML type in either, the
    // compiler has nothing else to tell them apart by.
    static_assert(!std::is_same_v<Texture, Font>, "Texture and Font must be distinct types");
    static_assert(sizeof(Texture) == sizeof(Font), "both handles are one pointer wide");

    CHECK(true);
}

// ---------------------------------------------------------------------------
// Moving
// ---------------------------------------------------------------------------

void testTextureMoveConstruction()
{
    Texture source;
    const Texture destination{std::move(source)};

    // Nothing to compare yet. What is being proved is that the operation exists,
    // is noexcept, and does not trip over a null implementation pointer, which
    // is the case a real resource move would exercise.
    (void)destination;
}

void testTextureMoveAssignment()
{
    Texture source;
    Texture destination;

    destination = std::move(source);

    (void)destination;
}

void testTextureSelfMoveAssignmentIsSafe()
{
    Texture texture;

    // Self-move is a bug everywhere except where it is deliberately made
    // harmless. std::swap and std::sort can produce one, and a handle that
    // corrupted itself or double-freed doing so would be a nightmare to trace.
    // Wrapped in a lambda so the warning-suppressing pragmas stay local.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    texture = std::move(texture);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    CHECK(true);
}

void testFontMoveConstruction()
{
    Font source;
    const Font destination{std::move(source)};

    (void)destination;
}

void testFontMoveAssignment()
{
    Font source;
    Font destination;

    destination = std::move(source);

    (void)destination;
}

void testFontSelfMoveAssignmentIsSafe()
{
    Font font;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    font = std::move(font);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    CHECK(true);
}

void testMoveAssignmentOntoAPopulatedTargetIsSafe()
{
    // Move assignment has to release whatever the destination already held before
    // taking over. With an empty implementation that is invisible, but the path
    // still has to run, and running it on a non-empty destination is the case
    // that would leak or double-free once a resource is actually held.
    // Self-move is covered by its own groups rather than repeated here.
    Texture populated;
    Texture incoming;

    populated = std::move(incoming);

    Font populatedFont;
    Font incomingFont;

    populatedFont = std::move(incomingFont);

    CHECK(true);
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void testDestructionIsSafe()
{
    // Scoped so destruction happens here, at the end of a group, under the
    // sanitizers. Every other group in this file destroys a handle too; this one
    // exists to make destruction an explicit, named thing under test.
    {
        Texture texture;
        Font font;
    }

    CHECK(true);
}

void testRepeatedMoveCycleIsSafe()
{
    // A long chain of hand-offs is what a real ownership transfer looks like, and
    // it is the shape that finds a stale pointer or a double free. Under ASan
    // and UBSan this is the group that would catch it.
    Texture texture;

    for (int i = 0; i < 1000; ++i)
    {
        Texture moved{std::move(texture)};
        texture = std::move(moved);
    }

    Font font;

    for (int i = 0; i < 1000; ++i)
    {
        Font moved{std::move(font)};
        font = std::move(moved);
    }

    CHECK(true);
}

void testIndependentDefaultHandlesCoexist()
{
    // Many handles, none of which knows about any other. The manager will hold a
    // whole library of these at once, so coexisting is the normal case rather
    // than an edge case.
    std::vector<Texture> textures;
    std::vector<Font> fonts;

    for (int i = 0; i < 32; ++i)
    {
        textures.emplace_back();
        fonts.emplace_back();
    }

    CHECK(textures.size() == 32U);
    CHECK(fonts.size() == 32U);

    // Distinct handles, distinct storage. If the two vectors somehow shared
    // anything, the sizes or the element addresses would give it away.
    const auto& firstTexture = reinterpret_cast<const void*>(&textures[0]);
    const auto& secondTexture = reinterpret_cast<const void*>(&textures[1]);
    const auto& firstFont = reinterpret_cast<const void*>(&fonts[0]);

    CHECK(firstTexture != secondTexture);
    CHECK(firstTexture != firstFont);
}

// ---------------------------------------------------------------------------
// Container and algorithm use
// ---------------------------------------------------------------------------

void testTextureHandlesCanBeHeldInAVector()
{
    // std::vector has to move on reallocation. This is the test that would fail
    // outright if the move constructor or assignment were ever removed, because
    // a non-movable, non-copyable type cannot be stored in a vector at all.
    std::vector<Texture> textures;
    textures.reserve(1);

    for (int i = 0; i < 64; ++i)
    {
        textures.emplace_back();
    }

    CHECK(textures.size() == 64U);

    // Force the moves to have actually happened during growth, then confirm the
    // elements are still there and still reachable.
    const Texture& front = textures.front();
    const Texture& back = textures.back();

    CHECK(&front != &back);
}

void testFontHandlesCanBeHeldInAVector()
{
    std::vector<Font> fonts;

    for (int i = 0; i < 64; ++i)
    {
        fonts.emplace_back();
    }

    CHECK(fonts.size() == 64U);
}

void testTextureHandlesSwap()
{
    // std::swap is implemented with moves and can produce a self-move. If a
    // handle mishandled that, the result would be a corrupted or double-freed
    // resource. std::sort is deliberately not used: it needs an operator<, and a
    // handle has no meaningful order, which the static_assert above pins down.
    std::vector<Texture> textures;
    for (int i = 0; i < 8; ++i)
    {
        textures.emplace_back();
    }

    std::swap(textures[0], textures[7]);
    std::swap(textures[2], textures[3]);

    CHECK(textures.size() == 8U);
}

void testFontHandlesSwap()
{
    std::vector<Font> fonts;
    for (int i = 0; i < 4; ++i)
    {
        fonts.emplace_back();
    }

    std::swap(fonts[0], fonts[3]);

    CHECK(fonts.size() == 4U);
}

// ---------------------------------------------------------------------------
// The SFML boundary
// ---------------------------------------------------------------------------

void testPublicHeadersContainNoSfmlReferences()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureHeader.empty());
    CHECK(!sources.fontHeader.empty());
    if (sources.textureHeader.empty() || sources.fontHeader.empty())
    {
        return;
    }

    // The point of these two types is that game code can include them without
    // reaching SFML. A header that mentioned a graphics type would compile
    // happily, pass every runtime test in this file, and still leak the whole
    // library into the public engine API.
    //
    // Comments have already been stripped, so the needles are plain substrings:
    // these headers talk about SFML on purpose, and only the code is under test.
    const std::vector<std::string> forbidden{"<SFML", "sf::", "SFML/"};

    CHECK_FALSE(containsAny(sources.textureHeader, forbidden));
    CHECK_FALSE(containsAny(sources.fontHeader, forbidden));
}

void testPublicHeadersDoNotForwardDeclareSfmlTypes()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureHeader.empty());
    CHECK(!sources.fontHeader.empty());
    if (sources.textureHeader.empty() || sources.fontHeader.empty())
    {
        return;
    }

    // Forward declaring `sf::Texture` or `sf::Font` in a public header would be
    // the easy way to satisfy the check above while still coupling the engine to
    // SFML's naming. The headers must not do it, and saying so by name is the
    // only way to catch it: a generic search for "sf::" would not distinguish a
    // forward declaration from any other mention of the namespace.
    const std::vector<std::string> forbidden{"namespace sf", "class Texture;", "class Font;", "struct Texture;",
                                             "struct Font;", "sf::Texture", "sf::Font"};

    CHECK_FALSE(containsAny(sources.textureHeader, forbidden));
    CHECK_FALSE(containsAny(sources.fontHeader, forbidden));
}

void testPublicHeadersIncludeNothingButMemory()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureHeader.empty());
    CHECK(!sources.fontHeader.empty());
    if (sources.textureHeader.empty() || sources.fontHeader.empty())
    {
        return;
    }

    // A whole-type check rather than a blacklist. An opaque handle has no reason
    // to include anything except the smart pointer it holds, so requiring the
    // includes to be exactly that one line catches a new dependency whatever it
    // happens to be, instead of only the ones somebody thought to forbid.
    //
    // The two headers are checked against their own expectations. Each one
    // declares its own class and neither declares the other, so a shared list
    // containing both names would fail for the wrong reason.
    const std::vector<std::string> shared{"#pragma once", "#include <memory>", "namespace engine::assets"};

    CHECK(containsAll(sources.textureHeader, shared));
    CHECK(containsAll(sources.fontHeader, shared));
    CHECK(containsAll(sources.textureHeader, {"class Texture"}));
    CHECK(containsAll(sources.fontHeader, {"class Font"}));

    // Neither header should mention the other's class, beyond the documentation
    // cross-reference that has just been stripped out.
    CHECK_FALSE(containsAny(sources.textureHeader, {"class Font"}));
    CHECK_FALSE(containsAny(sources.fontHeader, {"class Texture"}));

    // Exactly one include directive, and it is the smart pointer. Anything else
    // is a new dependency, and a blacklist would have to be updated every time
    // somebody reached for a different header.
    CHECK(countOccurrences(sources.textureHeader, "#include") == 1U);
    CHECK(countOccurrences(sources.fontHeader, "#include") == 1U);
}

void testPublicHeadersDeclareNoOtherDataMembers()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureHeader.empty());
    CHECK(!sources.fontHeader.empty());
    if (sources.textureHeader.empty() || sources.fontHeader.empty())
    {
        return;
    }

    // A handle that is supposed to hide its implementation should hold nothing
    // but the pointer to it. If a name, an identifier or a path ever appears as
    // a member, the type has stopped being an opaque resource and started
    // carrying configuration it was specifically designed not to own.
    const std::vector<std::string> forbidden{"std::string ", "std::filesystem", "std::ifstream", "std::size_t ",
                                             "std::uint32_t "};

    CHECK_FALSE(containsAny(sources.textureHeader, forbidden));
    CHECK_FALSE(containsAny(sources.fontHeader, forbidden));
}

void testImplementationsContainNoSfmlReferences()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureSource.empty());
    CHECK(!sources.fontSource.empty());
    if (sources.textureSource.empty() || sources.fontSource.empty())
    {
        return;
    }

    // The implementations are SFML-free too, and not by convention: they are
    // compiled into engine_assets, which links no graphics library, so an SFML
    // include here would fail to compile at all.
    const std::vector<std::string> forbidden{"<SFML", "sf::", "SFML/"};

    CHECK_FALSE(containsAny(sources.textureSource, forbidden));
    CHECK_FALSE(containsAny(sources.fontSource, forbidden));
}

void testImplementationsAreSelfContained()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureSource.empty());
    CHECK(!sources.fontSource.empty());
    if (sources.textureSource.empty() || sources.fontSource.empty())
    {
        return;
    }

    // Each .cpp must include only its own header, so there is no way for one to
    // start relying on something the other happened to include first.
    CHECK(containsAll(sources.textureSource, {"#include \"engine/assets/Texture.hpp\""}));
    CHECK(containsAll(sources.fontSource, {"#include \"engine/assets/Font.hpp\""}));

    // And the implementation type must be defined in the .cpp. It cannot be
    // forward declared in the header, so this is the only place it can live.
    // Finding it here is how the suite confirms the pimpl is real rather than
    // the implementation quietly inlined back into the public class.
    CHECK(containsAll(sources.textureSource, {"struct Texture::Impl"}));
    CHECK(containsAll(sources.fontSource, {"struct Font::Impl"}));
}

void testDestructorsAreDefinedOutOfLine()
{
    const Sources sources = loadSources();

    CHECK(!sources.textureSource.empty());
    CHECK(!sources.fontSource.empty());
    if (sources.textureSource.empty() || sources.fontSource.empty())
    {
        return;
    }

    // Destroying a unique_ptr needs a complete Impl. With the implementation
    // hidden in the .cpp, the destructor cannot be written in the header, and
    // this asserts it was not moved there to satisfy something else.
    CHECK(containsAll(sources.textureSource, {"Texture::~Texture()"}));
    CHECK(containsAll(sources.fontSource, {"Font::~Font()"}));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"texture can be default constructed", &testTextureCanBeDefaultConstructed},
        {"font can be default constructed", &testFontCanBeDefaultConstructed},
        {"texture and font are distinct types", &testTextureAndFontAreDistinctTypes},
        {"texture move construction", &testTextureMoveConstruction},
        {"texture move assignment", &testTextureMoveAssignment},
        {"texture self move assignment is safe", &testTextureSelfMoveAssignmentIsSafe},
        {"font move construction", &testFontMoveConstruction},
        {"font move assignment", &testFontMoveAssignment},
        {"font self move assignment is safe", &testFontSelfMoveAssignmentIsSafe},
        {"move assignment onto a populated target is safe", &testMoveAssignmentOntoAPopulatedTargetIsSafe},
        {"destruction is safe", &testDestructionIsSafe},
        {"repeated move cycle is safe", &testRepeatedMoveCycleIsSafe},
        {"independent default handles coexist", &testIndependentDefaultHandlesCoexist},
        {"texture handles can be held in a vector", &testTextureHandlesCanBeHeldInAVector},
        {"font handles can be held in a vector", &testFontHandlesCanBeHeldInAVector},
        {"texture handles swap", &testTextureHandlesSwap},
        {"font handles swap", &testFontHandlesSwap},
        {"public headers contain no sfml references", &testPublicHeadersContainNoSfmlReferences},
        {"public headers do not forward declare sfml types", &testPublicHeadersDoNotForwardDeclareSfmlTypes},
        {"public headers include nothing but memory", &testPublicHeadersIncludeNothingButMemory},
        {"public headers declare no other data members", &testPublicHeadersDeclareNoOtherDataMembers},
        {"implementations contain no sfml references", &testImplementationsContainNoSfmlReferences},
        {"implementations are self contained", &testImplementationsAreSelfContained},
        {"destructors are defined out of line", &testDestructorsAreDefinedOutOfLine},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        // These types do not throw today, but a group that did would be a
        // failure of that group rather than a reason to abort and hide the
        // result of every group after it.
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

    std::cout << groupCount << " asset handle test groups passed\n";
    return EXIT_SUCCESS;
}
