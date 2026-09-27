#include "engine/assets/AssetFile.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/Font.hpp"
#include "engine/assets/Texture.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
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

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)

using engine::assets::AssetManager;
using engine::assets::AssetNotFoundError;
using engine::assets::AssetParseError;
using engine::assets::Font;
using engine::assets::Texture;

// ---------------------------------------------------------------------------
// Compile-time guarantees: the interface contract.
//
// Step 4 adds no behaviour, so almost everything worth asserting about it is a
// property of its shape rather than of anything it does. These are static_asserts
// because shape is a whole-program concern: "lookups return a reference" has to
// hold for every caller, not merely for the one a runtime test happened to make.
// ---------------------------------------------------------------------------

// The interface must stay abstract. Giving a pure virtual a body would silently
// make every one of these types constructible, which is the first sign of a
// layering mistake.
static_assert(std::is_abstract_v<AssetManager>, "AssetManager must stay abstract");
static_assert(!std::is_default_constructible_v<AssetManager>, "AssetManager must not be constructible");
static_assert(!std::is_constructible_v<AssetManager, AssetManager>, "AssetManager must not be copy or move constructible");

// Deleting a concrete manager through this pointer is the normal lifetime path,
// so the destructor has to be virtual. Without it this compiles and then reads
// freed memory, which no runtime test in this file would reliably catch.
static_assert(std::has_virtual_destructor_v<AssetManager>, "AssetManager must have a virtual destructor");

// A manager owns a collection that other code holds references into, so it is
// never copied or moved.
static_assert(!std::is_copy_constructible_v<AssetManager>, "AssetManager must not be copy constructible");
static_assert(!std::is_copy_assignable_v<AssetManager>, "AssetManager must not be copy assignable");
static_assert(!std::is_move_constructible_v<AssetManager>, "AssetManager must not be move constructible");
static_assert(!std::is_move_assignable_v<AssetManager>, "AssetManager must not be move assignable");

// The two lookup signatures, spelled out. These pin the three decisions that the
// rest of the asset system is written against: lookups are const, they return a
// reference, and they return a reference to something const.
using TextureLookup = const Texture& (AssetManager::*)(std::string_view) const;
using FontLookup = const Font& (AssetManager::*)(std::string_view) const;
static_assert(std::is_same_v<decltype(&AssetManager::texture), TextureLookup>,
              "texture() must be a const member returning const Texture&");
static_assert(std::is_same_v<decltype(&AssetManager::font), FontLookup>,
              "font() must be a const member returning const Font&");

// Exactly two lookups and a destructor, and nothing else. This is the read-only
// promise, expressed as a shape: a third virtual function is a new capability,
// and the interface has none.
static_assert(std::is_invocable_v<TextureLookup, const AssetManager*, std::string_view>,
              "a const manager must be able to call the texture lookup");
static_assert(std::is_invocable_v<FontLookup, const AssetManager*, std::string_view>,
              "a const manager must be able to call the font lookup");

// A caller that gets a reference to a handle it does not own must not be able to
// write through it. A lookup returning a mutable reference would let any system
// mutate a resource shared with every other system, through a const manager. This
// checks const-correctness end to end, rather than trusting the member-pointer
// type alone: a non-const return would satisfy the signature above and fail here.
static_assert(std::is_same_v<decltype(std::declval<const AssetManager&>().texture(std::declval<std::string_view>())),
                             const Texture&>,
              "a lookup through a const manager must yield const Texture&, not a mutable reference");
static_assert(std::is_same_v<decltype(std::declval<const AssetManager&>().font(std::declval<std::string_view>())),
                             const Font&>,
              "a lookup through a const manager must yield const Font&, not a mutable reference");

// The two failure modes are separately catchable, because they are diagnosed in
// different places and need different fixes.
static_assert(std::is_base_of_v<std::out_of_range, AssetNotFoundError>,
              "AssetNotFoundError must be catchable as std::out_of_range, like std::map::at");
static_assert(std::is_base_of_v<std::exception, AssetNotFoundError>, "AssetNotFoundError must be an exception");
static_assert(std::is_constructible_v<AssetNotFoundError, std::string>, "AssetNotFoundError must carry a message");
static_assert(!std::is_base_of_v<AssetNotFoundError, AssetParseError>,
              "a missing asset is not a malformed configuration file");
static_assert(!std::is_base_of_v<AssetParseError, AssetNotFoundError>,
              "a malformed configuration file is not a missing asset");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A conforming implementation, written from outside the engine namespace to
/// prove the interface is actually implementable rather than merely declared.
///
/// It does what the interface says a manager must do — own its resources, hand
/// out stable references, throw on an unknown name — and nothing else. The
/// interface has no implementation of its own, so this stands in for one.
class FakeAssetManager final : public AssetManager
{
public:
    explicit FakeAssetManager(std::vector<std::size_t>* destructionLog) noexcept
        : m_destructionLog{destructionLog}
    {
    }

    ~FakeAssetManager() override
    {
        if (m_destructionLog != nullptr)
        {
            m_destructionLog->push_back(m_textureLookups + m_fontLookups);
        }
    }

    void addTexture(std::string name) { m_textures.emplace(std::move(name), Texture{}); }

    void addFont(std::string name) { m_fonts.emplace(std::move(name), Font{}); }

    const Texture& texture(const std::string_view name) const override
    {
        ++m_textureLookups;
        const auto found = m_textures.find(std::string{name});
        if (found == m_textures.end())
        {
            throw AssetNotFoundError{"no texture named '" + std::string{name} + "'"};
        }
        return found->second;
    }

    const Font& font(const std::string_view name) const override
    {
        ++m_fontLookups;
        const auto found = m_fonts.find(std::string{name});
        if (found == m_fonts.end())
        {
            throw AssetNotFoundError{"no font named '" + std::string{name} + "'"};
        }
        return found->second;
    }

private:
    std::map<std::string, Texture> m_textures;
    std::map<std::string, Font> m_fonts;
    std::vector<std::size_t>* m_destructionLog = nullptr;
    mutable std::size_t m_textureLookups = 0;
    mutable std::size_t m_fontLookups = 0;
};

/// Removes `//` comments and blank lines, leaving only code.
///
/// The interface's documentation explains at length that it is SFML-free, and
/// that explanation contains the substrings a search for SFML would look for.
/// The rule is about code, so comments are removed before anything is searched.
///
/// Block comments are not handled, and this header uses none.
[[nodiscard]] std::string stripComments(const std::string& source)
{
    std::string code;
    std::istringstream lines{source};
    std::string line;

    while (std::getline(lines, line))
    {
        const std::string withoutComment = line.substr(0, line.find("//"));

        if (withoutComment.find_first_not_of(" \t") != std::string::npos)
        {
            code += withoutComment;
            code += '\n';
        }
    }

    return code;
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

[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::string::size_type at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
    {
        ++count;
    }

    return count;
}

[[nodiscard]] std::string readInterfaceSource()
{
    const std::string path{ENGINE_ASSET_MANAGER_HEADER};
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

[[nodiscard]] std::string interfaceCode() { return stripComments(readInterfaceSource()); }

// ---------------------------------------------------------------------------
// The usage pattern
// ---------------------------------------------------------------------------

void testAnImplementationCanSatisfyTheInterface()
{
    // The reason the interface exists: a test, a tool or a future editor can
    // supply its own manager without the engine knowing or caring.
    std::vector<std::size_t> log;
    FakeAssetManager manager{&log};
    const AssetManager& asInterface = manager;

    manager.addTexture("mario");
    manager.addFont("debug");

    const Texture& texture = asInterface.texture("mario");
    const Font& font = asInterface.font("debug");

    (void)texture;
    (void)font;

    CHECK(true);
}

void testLookupsWorkThroughAConstReference()
{
    // How a system will actually use it: handed a const reference, reading only.
    // A non-const AssetManager& could not call these lookups at all if they were
    // anything but const, so the fact that this function compiles is itself part
    // of what is being asserted.
    std::vector<std::size_t> log;
    FakeAssetManager manager{&log};
    manager.addTexture("mario");
    manager.addFont("debug");

    const AssetManager& asInterface = manager;

    const Texture& texture = asInterface.texture("mario");
    const Font& font = asInterface.font("debug");

    // Two lookups of two different kinds must reach two different resources. The
    // addresses are the only thing observable about an opaque handle.
    CHECK(reinterpret_cast<const void*>(&texture) != reinterpret_cast<const void*>(&font));
}

void testLookupsReturnStableReferences()
{
    std::vector<std::size_t> log;
    FakeAssetManager manager{&log};
    manager.addTexture("mario");

    // The interface promises the same object on every call, which is what makes
    // it safe for a system to fetch a reference once and keep it. The assertion
    // that actually enforces this is the static_assert on the return type above;
    // by-value returns would fail to compile there. This check demonstrates what a
    // conforming implementation looks like, and is not the load-bearing test.
    const Texture& first = manager.texture("mario");
    const Texture& second = manager.texture("mario");

    CHECK(&first == &second);
}

void testAConcreteManagerCanBeDeletedThroughTheInterface()
{
    // What the virtual destructor is for. Without it this would read freed memory
    // and the test could still appear to pass, which is exactly why the
    // corresponding static_assert exists.
    std::vector<std::size_t> log;

    {
        FakeAssetManager* concrete = new FakeAssetManager{&log};
        concrete->addTexture("mario");
        (void)concrete->texture("mario");

        AssetManager* asInterface = concrete;
        delete asInterface;
    }

    // The derived destructor ran, recording the number of lookups it had served.
    CHECK(log.size() == 1U);
    if (!log.empty())
    {
        CHECK(log.front() == 1U);
    }
}

void testAMissingNameIsReportedDistinctly()
{
    std::vector<std::size_t> log;
    FakeAssetManager manager{&log};
    manager.addTexture("mario");

    bool sawNotFound = false;
    bool sawParseError = false;

    try
    {
        (void)manager.texture("missing");
    }
    catch (const AssetNotFoundError& error)
    {
        sawNotFound = true;
        CHECK(std::string_view{error.what()}.find("missing") != std::string_view::npos);
    }
    catch (const AssetParseError&)
    {
        sawParseError = true;
    }

    CHECK(sawNotFound);
    // Catching the wrong family would be a regression in the diagnostics, so the
    // one error must not fall through into the other.
    CHECK_FALSE(sawParseError);
}

void testParseErrorsAreStillCaughtSeparately()
{
    // The other half of the same separation: a malformed file and a missing name
    // are reachable and independently catchable.
    bool sawParseError = false;

    try
    {
        (void)engine::assets::parseAssetFile("Sound beep beep.wav\n");
    }
    catch (const AssetParseError&)
    {
        sawParseError = true;
    }
    catch (const AssetNotFoundError&)
    {
        CHECK_FALSE(true); // the wrong family caught it
    }

    CHECK(sawParseError);
}

// ---------------------------------------------------------------------------
// The SFML boundary
// ---------------------------------------------------------------------------

void testInterfaceHeaderContainsNoSfmlReferences()
{
    const std::string code = interfaceCode();

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // A header that reached SFML would compile, pass every runtime check in this
    // file, and still hand the whole graphics library to every game that included
    // it. That is the specific thing this interface has to prevent, and it is why
    // the check is on the text: no behaviour test can observe it.
    const std::vector<std::string> forbidden{"<SFML", "sf::", "SFML/", "namespace sf"};

    CHECK_FALSE(containsAny(code, forbidden));
}

void testInterfaceHeaderIncludesOnlySfmlFreeEngineHeaders()
{
    const std::string code = interfaceCode();

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // The two handle headers and the standard exception and string headers. The
    // handles are the whole point: they are what make a return type possible
    // without naming a graphics type.
    CHECK(code.find("#include \"engine/assets/Font.hpp\"") != std::string::npos);
    CHECK(code.find("#include \"engine/assets/Texture.hpp\"") != std::string::npos);
    CHECK(code.find("#include <stdexcept>") != std::string::npos);
    CHECK(code.find("#include <string_view>") != std::string::npos);

    // Exactly those four. A new dependency should have to be added here
    // deliberately, rather than arriving because something else needed it.
    CHECK(countOccurrences(code, "#include") == 4U);
}

void testInterfaceHeaderHasNoLoadingOrMutation()
{
    const std::string code = interfaceCode();

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // The read-only promise, checked as a shape. A destructor and two lookups is
    // the entire surface: three virtual declarations and two pure ones. Any new
    // capability - a load, a reload, a clear, a setter - changes these counts and
    // fails here, which is the point. It is what stops a reload being added later
    // without anyone considering the dangling references it would cause.
    CHECK(countOccurrences(code, "virtual") == 3U);
    CHECK(countOccurrences(code, "= 0;") == 2U);

    // Spelled out as well, so the failure says which capability appeared rather
    // than just reporting a count.
    const std::vector<std::string> forbidden{"load(", "reload(", "clear(", "insert(", "add(", "set(", "remove("};

    CHECK_FALSE(containsAny(code, forbidden));
}

void testInterfaceHeaderDeclaresNoDataMembers()
{
    const std::string code = interfaceCode();

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // An abstract interface holds no state. A member here would mean the
    // collection had become part of the abstraction, and the read-only guarantee
    // about reference stability would be a comment rather than a fact. A container
    // member and a plain scalar are both listed, because a scalar is the easier
    // one to add by accident.
    const std::vector<std::string> forbidden{"std::map", "std::unordered_map", "std::vector", "std::string m_",
                                             "std::size_t m_", "int m_", "bool m_", "mutable "};

    CHECK_FALSE(containsAny(code, forbidden));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"an implementation can satisfy the interface", &testAnImplementationCanSatisfyTheInterface},
        {"lookups work through a const reference", &testLookupsWorkThroughAConstReference},
        {"lookups return stable references", &testLookupsReturnStableReferences},
        {"a concrete manager can be deleted through the interface", &testAConcreteManagerCanBeDeletedThroughTheInterface},
        {"a missing name is reported distinctly", &testAMissingNameIsReportedDistinctly},
        {"parse errors are still caught separately", &testParseErrorsAreStillCaughtSeparately},
        {"interface header contains no sfml references", &testInterfaceHeaderContainsNoSfmlReferences},
        {"interface header includes only sfml-free engine headers", &testInterfaceHeaderIncludesOnlySfmlFreeEngineHeaders},
        {"interface header has no loading or mutation", &testInterfaceHeaderHasNoLoadingOrMutation},
        {"interface header declares no data members", &testInterfaceHeaderDeclaresNoDataMembers},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        // Lookups throw by contract, so an exception escaping a group is a failure
        // of that group rather than a reason to abort and hide the rest.
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

    std::cout << groupCount << " asset manager interface test groups passed\n";
    return EXIT_SUCCESS;
}
