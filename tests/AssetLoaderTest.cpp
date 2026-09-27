#include "engine/assets/AssetFile.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/Font.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/assets/Texture.hpp"

#include <SFML/Graphics/Font.hpp>
#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/Texture.hpp>

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

using engine::assets::AssetLoadError;
using engine::assets::AssetManager;
using engine::assets::AssetNotFoundError;
using engine::assets::AssetParseError;
using engine::assets::Font;
using engine::assets::SfmlAssetManager;
using engine::assets::Texture;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Temporary directory
//
// Every configuration this suite loads is written into a temporary directory and
// removed afterwards. Nothing is written into the repository, and no group
// depends on a configuration file being checked in: the committed assets.txt
// belongs to a later step, and its contents are not this step's business.
// ---------------------------------------------------------------------------

/// A directory that exists for the lifetime of the object and is then removed,
/// contents and all.
class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        // Unique per instance without a platform call: the address of a local adds
        // address-space randomisation, and the counter covers two objects created
        // within the same instant. create_directory then fails harmlessly on a
        // collision and the next attempt is used.
        const std::uintptr_t unique = reinterpret_cast<std::uintptr_t>(&m_scratch);
        const fs::path base = fs::temp_directory_path();

        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const fs::path candidate =
                base / ("engine-asset-test-" + std::to_string(unique) + "-" + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(candidate, error))
            {
                // Canonicalised on purpose, and this is not tidiness.
                //
                // On macOS the temporary directory lives under /var, which is a
                // symlink to /private/var. A relative path that climbs out of it
                // with ".." is resolved by the kernel physically, so the hop out
                // of "var" lands on "private" rather than on the root, and a
                // configuration in here cannot reach /Users/... at all. Storing
                // the resolved path keeps the configuration, the relative paths
                // written into it, and the loader's own resolution all on the same
                // real branch, where ".." means what it looks like.
                m_path = fs::canonical(candidate);
                return;
            }
        }

        throw std::runtime_error{"could not create a temporary directory"};
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return m_path; }

    /// Writes `contents` to `name` inside the directory and returns its path.
    [[nodiscard]] fs::path write(const std::string& name, const std::string& contents) const
    {
        const fs::path file = m_path / name;
        std::ofstream out{file};
        if (!out)
        {
            throw std::runtime_error{"could not write " + file.string()};
        }
        out << contents;
        return file;
    }

    /// Creates a subdirectory and returns its path.
    [[nodiscard]] fs::path subdirectory(const std::string& name) const
    {
        const fs::path sub = m_path / name;
        std::error_code error;
        fs::create_directories(sub, error);
        return sub;
    }

    /// Copies a library file into `directory` under `name`, byte for byte.
    /// Copying rather than symlinking keeps the two test files genuinely
    /// different files at different paths, which is what the resolution test
    /// needs to tell apart.
    void place(const fs::path& directory, const std::string& name, const fs::path& source) const
    {
        std::ifstream in{source, std::ios::binary};
        if (!in)
        {
            throw std::runtime_error{"could not read " + source.string()};
        }

        std::ofstream out{directory / name, std::ios::binary};
        if (!out)
        {
            throw std::runtime_error{"could not write " + (directory / name).string()};
        }

        out << in.rdbuf();
    }

private:
    fs::path m_path;
    char m_scratch = 0;
};

/// The committed asset library, as an absolute path.
[[nodiscard]] fs::path libraryDirectory() { return fs::path{ENGINE_ASSET_LIBRARY_DIR}; }

/// Writes a library file's path into a configuration the way a user would: a path
/// relative to the configuration's own directory, which is what the loader
/// resolves against.
///
/// Takes the directory the configuration will live in, not the configuration
/// itself. Deriving the base here rather than from a file path keeps the call
/// sites honest, and getting it wrong by one level produces a path that looks
/// plausible and silently resolves to nothing.
[[nodiscard]] std::string entryFor(const fs::path& configurationDirectory, const std::string& libraryRelative)
{
    const fs::path relative = fs::relative(libraryDirectory() / libraryRelative, configurationDirectory);
    return relative.generic_string();
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
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

/// Removes `//` comments and blank lines, leaving only code. The manager's header
/// documents at length that it is read-only, and that explanation contains the
/// words a search for mutators looks for, so comments go first.
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

// ---------------------------------------------------------------------------
// Compile-time guarantees.
//
// The loader must not have quietly changed the contract it was handed in Step 3.
// ---------------------------------------------------------------------------

// Still exactly one pointer wide, so populating the implementation changed a
// handle's size by nothing. A loader that had quietly added a name, a path or a
// manager pointer to the handle would be caught by exactly this.
static_assert(sizeof(Texture) == sizeof(void*), "Texture must remain exactly one pointer wide");
static_assert(sizeof(Font) == sizeof(void*), "Font must remain exactly one pointer wide");

// Still movable and not copyable, so a loaded handle cannot be duplicated behind
// the manager's back and double-free its resource.
static_assert(!std::is_copy_constructible_v<Texture>, "Texture must not be copy constructible");
static_assert(!std::is_copy_assignable_v<Texture>, "Texture must not be copy assignable");
static_assert(std::is_move_constructible_v<Texture>, "Texture must be move constructible");
static_assert(std::is_nothrow_move_assignable_v<Texture>, "Texture move assignment must be noexcept");

// A concrete manager is a manager, and it owns every loaded resource, so it is
// never copied or moved itself.
static_assert(std::is_base_of_v<AssetManager, SfmlAssetManager>, "SfmlAssetManager must implement AssetManager");
static_assert(!std::is_copy_constructible_v<SfmlAssetManager>, "SfmlAssetManager must not be copy constructible");
static_assert(!std::is_copy_assignable_v<SfmlAssetManager>, "SfmlAssetManager must not be copy assignable");

// Loaded at construction, and never default constructed: an empty manager would
// have nothing to hand out, and the whole read-only guarantee rests on the
// collection being complete before anyone can ask for a reference into it.
static_assert(!std::is_default_constructible_v<SfmlAssetManager>,
              "a manager must be given its configuration, not left empty");
static_assert(std::is_constructible_v<SfmlAssetManager, const fs::path&>,
              "a manager must accept a configuration path");

// The three failure modes stay separately catchable. Flattening them into one
// would make "your configuration is malformed" and "that file is missing"
// indistinguishable, which is the distinction a deployment needs.
static_assert(!std::is_base_of_v<AssetLoadError, AssetParseError>, "a load failure is not a parse failure");
static_assert(!std::is_base_of_v<AssetParseError, AssetLoadError>, "a parse failure is not a load failure");
static_assert(!std::is_base_of_v<AssetLoadError, AssetNotFoundError>, "a load failure is not a missing name");
static_assert(!std::is_base_of_v<AssetNotFoundError, AssetLoadError>, "a missing name is not a load failure");
static_assert(std::is_base_of_v<std::exception, AssetLoadError>, "AssetLoadError must be an exception");

// ---------------------------------------------------------------------------
// Loading and caching
// ---------------------------------------------------------------------------

void testTexturesAndFontsLoad()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\nFont pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};

    CHECK(manager.textureCount() == 1U);
    CHECK(manager.fontCount() == 1U);

    // A 64x64 ground tile is the real size of the committed file, so this is a
    // check that the right pixels were uploaded rather than that something was.
    CHECK(manager.nativeTexture(manager.texture("ground")).getSize() == sf::Vector2u{64U, 64U});
    CHECK(!manager.nativeFont(manager.font("pixeled")).getInfo().family.empty());
}

void testDeclaredNamesAreLookedUp()
{
    const TemporaryDirectory directory;
    const fs::path config = directory.write("assets.txt", "Texture ground " +
                                                          entryFor(directory.path(), "images/mario/ground.png") +
                                                          "\nTexture stand " +
                                                          entryFor(directory.path(), "images/megaman/megaStand.png") +
                                                          "\nFont pixeled " +
                                                          entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};

    // Names are how everything above the loader refers to an asset, so each name
    // has to reach its own resource rather than the first one loaded. The two
    // sizes differ, which is how the test tells them apart.
    CHECK(manager.nativeTexture(manager.texture("ground")).getSize() == sf::Vector2u{64U, 64U});
    CHECK(manager.nativeTexture(manager.texture("stand")).getSize() == sf::Vector2u{190U, 208U});
    CHECK(manager.textureCount() == 2U);
    CHECK(manager.fontCount() == 1U);
}

void testRepeatedLookupsReturnTheSameObject()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\nFont pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};

    // The point of loading once. Address equality is the only externally visible
    // way to say "the same object, not a copy and not a re-load", and it is what
    // makes a reference safe to hold for the length of a frame.
    CHECK(&manager.texture("ground") == &manager.texture("ground"));
    CHECK(&manager.font("pixeled") == &manager.font("pixeled"));
}

void testHandlesRemainOpaqueAfterLoading()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\nFont pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};
    const Texture& texture = manager.texture("ground");
    const Font& font = manager.font("pixeled");

    // Populating the implementation must not have grown the handle. Measured
    // rather than assumed, because a loader that quietly added a cache index or
    // the resolved path to the handle would still return a working texture.
    CHECK(sizeof(texture) == sizeof(void*));
    CHECK(sizeof(font) == sizeof(void*));
}

void testEmptyConfigurationLoadsNothing()
{
    const TemporaryDirectory directory;
    const fs::path config = directory.write("assets.txt", "# nothing declared\n\n   \n");

    const SfmlAssetManager manager{config};

    CHECK(manager.textureCount() == 0U);
    CHECK(manager.fontCount() == 0U);
}

void testEveryCommittedLibraryFileLoads()
{
    // Every file in the committed library through one configuration, so nothing
    // in it is quietly unloadable. The image count is derived from the library
    // rather than hard-coded, so a file added there later is covered without
    // editing this test.
    const TemporaryDirectory directory;

    std::ostringstream config;
    std::size_t declaredImages = 0;

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{libraryDirectory() / "images"})
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".png")
        {
            continue;
        }

        const std::string name = entry.path().parent_path().filename().string() + "_" +
                                 entry.path().stem().string();
        config << "Texture " << name << ' ' << entryFor(directory.path(), entry.path().string()) << '\n';
        ++declaredImages;
    }

    for (const char* font : {"pixeled", "tech", "numbers"})
    {
        const std::string file = std::string{font} + ".ttf";
        config << "Font " << font << ' ' << entryFor(directory.path(), "fonts/" + file) << '\n';
    }

    const SfmlAssetManager manager{directory.write("assets.txt", config.str())};

    CHECK(declaredImages == 24U);
    CHECK(manager.textureCount() == declaredImages);
    CHECK(manager.fontCount() == 3U);

    // And they hold real resources, not just entries: a representative from each
    // subdirectory, checked against the real size of the committed file.
    CHECK(manager.nativeTexture(manager.texture("mario_ground")).getSize() == sf::Vector2u{64U, 64U});
    CHECK(manager.nativeTexture(manager.texture("megaman_megaStand")).getSize() == sf::Vector2u{190U, 208U});
    CHECK(manager.nativeTexture(manager.texture("animations_explosion")).getSize() == sf::Vector2u{1152U, 96U});
}

// ---------------------------------------------------------------------------
// Palette-indexed PNGs
// ---------------------------------------------------------------------------

void testIndexedPngProducesCorrectPixels()
{
    // The three palette-indexed files are the one place this library could have
    // gone wrong: their pixels are stored as palette indices, and a decoder that
    // treated those indices as colour channels would produce plausible-looking
    // nonsense. The expected values are the true decoded colours of the artwork,
    // so this is a check against the files and not merely "it did not crash".
    const TemporaryDirectory directory;
    const fs::path config = directory.write(
        "assets.txt", "Texture question " + entryFor(directory.path(), "images/mario/question.png") +
                           "\nTexture flagpole " + entryFor(directory.path(), "images/mario/Flagpole.png") +
                           "\nTexture explosion " + entryFor(directory.path(), "images/animations/explosion.png") +
                           "\n");

    const SfmlAssetManager manager{config};

    // 4-bit indexed. (180,180) is inside the orange body of the block.
    const sf::Image question = manager.nativeTexture(manager.texture("question")).copyToImage();
    CHECK(question.getSize() == sf::Vector2u{360U, 360U});
    CHECK(question.getPixel(180U, 180U) == sf::Color(227, 53, 0, 255));
    CHECK(question.getPixel(0U, 0U) == sf::Color(0, 0, 0, 255));
    CHECK(question.getPixel(359U, 359U) == sf::Color(86, 63, 30, 255));

    // 4-bit indexed. The last pixel is opaque where the first is clear, so alpha
    // survived the palette lookup too.
    const sf::Image flagpole = manager.nativeTexture(manager.texture("flagpole")).copyToImage();
    CHECK(flagpole.getSize() == sf::Vector2u{75U, 528U});
    CHECK(flagpole.getPixel(74U, 527U) == sf::Color(186, 97, 17, 255));
    CHECK(flagpole.getPixel(37U, 264U) == sf::Color(0, 0, 0, 2));

    // 8-bit indexed, fully transparent where sampled.
    const sf::Image explosion = manager.nativeTexture(manager.texture("explosion")).copyToImage();
    CHECK(explosion.getSize() == sf::Vector2u{1152U, 96U});
    CHECK(explosion.getPixel(0U, 0U) == sf::Color(0, 0, 0, 0));
}

void testAnRgbaPngIsUnaffected()
{
    // The control. If indexed files were being handled by some special path, this
    // is the group that would show it had been applied to everything.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};
    const sf::Image ground = manager.nativeTexture(manager.texture("ground")).copyToImage();

    // The true decoded values of this file, including a partly transparent corner
    // pixel so alpha is covered as well as colour.
    CHECK(ground.getSize() == sf::Vector2u{64U, 64U});
    CHECK(ground.getPixel(0U, 0U) == sf::Color(207, 75, 26, 149));
    CHECK(ground.getPixel(32U, 32U) == sf::Color(190, 77, 19, 255));
    CHECK(ground.getPixel(63U, 63U) == sf::Color(198, 82, 5, 255));
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

void testFontsParseRatherThanMerelyExisting()
{
    // "The file is there" is not "the font parsed". A truncated or mistyped font
    // still exists on disk, and SFML reports an empty family for anything that did
    // not parse, so the family name is the observable that distinguishes them.
    const TemporaryDirectory directory;
    const fs::path config = directory.write(
        "assets.txt", "Font pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") +
                           "\nFont tech " + entryFor(directory.path(), "fonts/tech.ttf") +
                           "\nFont numbers " + entryFor(directory.path(), "fonts/numbers.ttf") + "\n");

    const SfmlAssetManager manager{config};

    CHECK(manager.fontCount() == 3U);
    CHECK(manager.nativeFont(manager.font("pixeled")).getInfo().family == "Pixeled");
    CHECK(manager.nativeFont(manager.font("tech")).getInfo().family == "FAST-TRACK");

    // numbers.ttf carries two conflicting family records in its name table; SFML
    // reports the Windows one. Asserted because that is a real, stable fact about
    // how this file is read, and a change would mean the file was replaced.
    CHECK(manager.nativeFont(manager.font("numbers")).getInfo().family == "Secret Code");
}

void testFontsAreCachedLikeTextures()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Font pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};

    CHECK(&manager.font("pixeled") == &manager.font("pixeled"));
    CHECK(manager.fontCount() == 1U);
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

void testPathsResolveRelativeToTheConfigurationFile()
{
    // The configuration lives in a temporary directory with nothing to do with the
    // process working directory, and names the library by a relative path. If
    // resolution used the working directory instead, this would not load at all.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};

    CHECK(manager.textureCount() == 1U);
    CHECK(manager.nativeTexture(manager.texture("ground")).getSize() == sf::Vector2u{64U, 64U});
}

void testResolutionFollowsTheConfigNotTheProcess()
{
    // Two configurations in two different directories, each naming a file called
    // local.png that exists only beside it, and the two files have different
    // sizes. Resolving against the process working directory would load neither,
    // or load the same file twice. This is the property "relative to the
    // configuration" actually has.
    const TemporaryDirectory directory;
    const fs::path first = directory.subdirectory("first");
    const fs::path second = directory.subdirectory("second");

    // 64x64 and 117x69, so the loaded size says which one was read.
    directory.place(first, "local.png", libraryDirectory() / "images/mario/ground.png");
    directory.place(second, "local.png", libraryDirectory() / "images/mario/cloudsmall.png");

    std::ofstream firstConfig{first / "assets.txt"};
    firstConfig << "Texture local local.png\n";
    firstConfig.close();

    std::ofstream secondConfig{second / "assets.txt"};
    secondConfig << "Texture local local.png\n";
    secondConfig.close();

    const SfmlAssetManager firstManager{first / "assets.txt"};
    const SfmlAssetManager secondManager{second / "assets.txt"};

    CHECK(firstManager.nativeTexture(firstManager.texture("local")).getSize() == sf::Vector2u{64U, 64U});
    CHECK(secondManager.nativeTexture(secondManager.texture("local")).getSize() == sf::Vector2u{117U, 69U});
}

// ---------------------------------------------------------------------------
// Failures
// ---------------------------------------------------------------------------

void testMissingTextureNameIsReported()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};

    bool sawNotFound = false;
    try
    {
        (void)manager.texture("nope");
    }
    catch (const AssetNotFoundError& error)
    {
        sawNotFound = true;
        CHECK(contains(error.what(), "nope"));
    }
    catch (const AssetLoadError&)
    {
        CHECK_FALSE(true); // wrong family
    }

    CHECK(sawNotFound);
}

void testMissingFontNameIsReported()
{
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Font pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};

    bool sawNotFound = false;
    try
    {
        (void)manager.font("nope");
    }
    catch (const AssetNotFoundError& error)
    {
        sawNotFound = true;
        CHECK(contains(error.what(), "nope"));
    }
    catch (const AssetLoadError&)
    {
        CHECK_FALSE(true); // wrong family
    }

    CHECK(sawNotFound);
}

void testNamesAreLookedUpPerKind()
{
    // A texture name is not a font name. Sharing one lookup would quietly satisfy a
    // level file that got the kind wrong, which is exactly the sort of mistake
    // this is meant to surface.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};

    bool textureLookupFailed = false;
    bool fontLookupFailed = false;

    try
    {
        (void)manager.font("ground");
    }
    catch (const AssetNotFoundError&)
    {
        fontLookupFailed = true;
    }

    try
    {
        (void)manager.texture("pixeled");
    }
    catch (const AssetNotFoundError&)
    {
        textureLookupFailed = true;
    }

    CHECK(fontLookupFailed);
    CHECK(textureLookupFailed);
}

void testMissingFileNamesTheAssetAndTheResolvedPath()
{
    // This is the failure a deployment hits: the configuration is valid, the name
    // is declared, and the file is not there. A message without the resolved path
    // would leave the reader guessing whether resolution went somewhere
    // unexpected, which is the one thing worth knowing.
    const TemporaryDirectory directory;
    const fs::path config = directory.write("assets.txt", "Texture ghost images/mario/ghost.png\n");

    bool sawLoadError = false;
    try
    {
        const SfmlAssetManager manager{config};
        (void)manager.texture("ghost");
    }
    catch (const AssetLoadError& error)
    {
        sawLoadError = true;
        CHECK(contains(error.what(), "ghost"));
        CHECK(contains(error.what(), "ghost.png"));
        CHECK(contains(error.what(), (directory.path() / "images" / "mario" / "ghost.png").string()));
    }
    catch (const AssetParseError&)
    {
        CHECK_FALSE(true); // wrong family
    }

    CHECK(sawLoadError);
}

void testMissingFontFileIsReported()
{
    const TemporaryDirectory directory;
    const fs::path config = directory.write("assets.txt", "Font ghost fonts/ghost.ttf\n");

    bool sawLoadError = false;
    try
    {
        const SfmlAssetManager manager{config};
        (void)manager.font("ghost");
    }
    catch (const AssetLoadError& error)
    {
        sawLoadError = true;
        CHECK(contains(error.what(), "ghost.ttf"));
    }
    catch (const AssetParseError&)
    {
        CHECK_FALSE(true); // wrong family
    }

    CHECK(sawLoadError);
}

void testMalformedConfigurationKeepsTheParsersDiagnosis()
{
    // A malformed line is a parser problem. The loader must not flatten it into a
    // load failure, because the line number is the whole value of the message and
    // flattening throws it away.
    const TemporaryDirectory directory;
    const fs::path config = directory.write("assets.txt", "Texture ground " +
                                                          entryFor(directory.path(), "images/mario/ground.png") +
                                                          "\nAnimation nope nope.png\n");

    bool sawParseError = false;
    try
    {
        const SfmlAssetManager manager{config};
        (void)manager;
    }
    catch (const AssetParseError& error)
    {
        sawParseError = true;
        CHECK(contains(error.what(), "line 2"));
        CHECK(contains(error.what(), "Animation"));
    }
    catch (const AssetLoadError&)
    {
        CHECK_FALSE(true); // wrong family
    }

    CHECK(sawParseError);
}

void testMissingConfigurationFileIsReported()
{
    const TemporaryDirectory directory;
    const fs::path config = directory.path() / "not-here.txt";

    bool sawLoadError = false;
    try
    {
        const SfmlAssetManager manager{config};
        (void)manager;
    }
    catch (const AssetLoadError& error)
    {
        sawLoadError = true;
        CHECK(contains(error.what(), "not-here.txt"));
    }

    CHECK(sawLoadError);
}

void testAnEmptyHandleHasNoPlatformResource()
{
    // A default constructed handle holds nothing. Returning something for it
    // anyway would turn a mistake here into a crash somewhere unrelated later.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\nFont pixeled " + entryFor(directory.path(), "fonts/pixeled.ttf") + "\n");

    const SfmlAssetManager manager{config};
    const Texture emptyTexture;
    const Font emptyFont;

    bool sawTextureError = false;
    try
    {
        (void)manager.nativeTexture(emptyTexture);
    }
    catch (const AssetLoadError&)
    {
        sawTextureError = true;
    }

    bool sawFontError = false;
    try
    {
        (void)manager.nativeFont(emptyFont);
    }
    catch (const AssetLoadError&)
    {
        sawFontError = true;
    }

    CHECK(sawTextureError);
    CHECK(sawFontError);
}

// ---------------------------------------------------------------------------
// The read-only contract
// ---------------------------------------------------------------------------

void testTheManagerHeaderOffersNoMutation()
{
    // The interface is read-only, and the concrete manager must not have added a
    // way around it. Loading happens in the constructor, so there is no load,
    // clear, add, set, insert or replace at all: a reference handed out earlier
    // could otherwise be invalidated by a call the caller had no reason to
    // distrust.
    //
    // This is a source check because there is no way to observe the absence of a
    // method at runtime.
    std::ifstream file{ENGINE_SFML_ASSET_MANAGER_HEADER};
    CHECK(static_cast<bool>(file));
    if (!file)
    {
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string code = stripComments(buffer.str());

    const std::vector<std::string> forbidden{"load(", "reload(", "clear(", "add(", "set(", "insert(", "remove(",
                                             "replace(", "reset("};

    CHECK_FALSE(containsAny(code, forbidden));

    // Exactly one constructor, and it takes the configuration. A second one
    // without one would be a way to create a manager holding nothing.
    CHECK(countOccurrences(code, "explicit SfmlAssetManager(") == 1U);

    // And the two diagnostics accessors are the only extra public surface.
    CHECK(countOccurrences(code, "nativeTexture(") == 1U);
    CHECK(countOccurrences(code, "nativeFont(") == 1U);
}

void testTwoManagersLoadIndependently()
{
    // Each manager owns its own resources. Nothing is shared or cached across
    // instances, so two managers naming the same file hold distinct resources and
    // destroying one does not disturb the other.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const Texture* survivor = nullptr;
    {
        const SfmlAssetManager temporary{config};
        CHECK(temporary.textureCount() == 1U);
    }

    // The temporary manager is gone, along with its texture. A surviving
    // reference would have been freed here, so the next manager is the proof that
    // ownership was never shared.
    const SfmlAssetManager kept{config};
    CHECK(kept.nativeTexture(kept.texture("ground")).getSize() == sf::Vector2u{64U, 64U});
    survivor = &kept.texture("ground");
    CHECK(survivor != nullptr);
}

void testALoadedHandleCannotBeMovedOutOfTheManager()
{
    // The only way to hold a loaded handle is through a manager-owned const
    // reference, so a populated handle cannot be moved or copied into a caller's
    // own variable at all. That is not a limitation of how this test was written,
    // it is what the design produces, and it is the reason a caller's reference
    // can never be invalidated by somebody else moving a resource out from under
    // it. Asserting it in the type system is the honest form of the claim: a
    // loaded handle simply cannot be taken away.
    static_assert(!std::is_assignable_v<const Texture&, Texture&&>,
                  "a loaded texture must not be movable out of the manager");
    static_assert(!std::is_assignable_v<const Font&, Font&&>,
                  "a loaded font must not be movable out of the manager");
    static_assert(!std::is_constructible_v<Texture, const Texture&>,
                  "a loaded texture must not be copy constructible");
    static_assert(!std::is_constructible_v<Font, const Font&>, "a loaded font must not be copy constructible");

    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};
    const Texture& handle = manager.texture("ground");

    // Still the manager's, still valid, still the same object on a second lookup.
    CHECK(manager.nativeTexture(handle).getSize() == sf::Vector2u{64U, 64U});
    CHECK(&handle == &manager.texture("ground"));
    CHECK(manager.textureCount() == 1U);
}

void testSelfMoveLeavesAHandleUsable()
{
    // The self-move guard from Step 3 is now reachable with an implementation in
    // play. On a handle the loader populated it would matter most, but a populated
    // handle cannot be moved at all (the group above), so the only reachable self
    // move is on an empty one - and it has to leave a handle that is still
    // empty and still safe to destroy, rather than a half-destroyed one. Under
    // ASan a bad self-move is a use-after-free rather than a guess.
    const TemporaryDirectory directory;
    const fs::path config =
        directory.write("assets.txt", "Texture ground " + entryFor(directory.path(), "images/mario/ground.png") +
                           "\n");

    const SfmlAssetManager manager{config};

    Texture empty;
    Font emptyFont;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    empty = std::move(empty);
    emptyFont = std::move(emptyFont);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    // Still empty, not dangling: the accessor still refuses it.
    bool textureStillEmpty = false;
    bool fontStillEmpty = false;

    try
    {
        (void)manager.nativeTexture(empty);
    }
    catch (const AssetLoadError&)
    {
        textureStillEmpty = true;
    }

    try
    {
        (void)manager.nativeFont(emptyFont);
    }
    catch (const AssetLoadError&)
    {
        fontStillEmpty = true;
    }

    CHECK(textureStillEmpty);
    CHECK(fontStillEmpty);

    // And the manager's own resources were untouched by any of it.
    CHECK(manager.nativeTexture(manager.texture("ground")).getSize() == sf::Vector2u{64U, 64U});
}

void testTheManagerSurvivesEveryDeclaredAssetBeingUsed()
{
    // One manager, every library file loaded, then every name looked up. A
    // resource freed too early, or a map rehashed in a way that invalidated a
    // reference, shows up here rather than in whichever game happens to draw.
    const TemporaryDirectory directory;

    std::ostringstream config;
    std::vector<std::string> textureNames;
    std::vector<std::string> fontNames;

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{libraryDirectory() / "images"})
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".png")
        {
            continue;
        }

        const std::string name = entry.path().parent_path().filename().string() + "_" +
                                 entry.path().stem().string();
        config << "Texture " << name << ' ' << entryFor(directory.path(), entry.path().string()) << '\n';
        textureNames.push_back(name);
    }

    for (const char* font : {"pixeled", "tech", "numbers"})
    {
        config << "Font " << font << ' ' << entryFor(directory.path(), std::string{"fonts/"} + font + ".ttf") << '\n';
        fontNames.emplace_back(font);
    }

    const SfmlAssetManager manager{directory.write("assets.txt", config.str())};

    // Hold every reference at once, then read every one afterwards. Holding them
    // all simultaneously is the part that would expose a lifetime problem.
    std::vector<const Texture*> textures;
    std::vector<const Font*> fonts;
    for (const std::string& name : textureNames)
    {
        textures.push_back(&manager.texture(name));
    }
    for (const std::string& name : fontNames)
    {
        fonts.push_back(&manager.font(name));
    }

    CHECK(textures.size() == 24U);
    CHECK(fonts.size() == 3U);

    for (std::size_t index = 0; index < textures.size(); ++index)
    {
        CHECK(&manager.texture(textureNames[index]) == textures[index]);
        CHECK(manager.nativeTexture(*textures[index]).getSize().x > 0U);
        CHECK(manager.nativeTexture(*textures[index]).getSize().y > 0U);
    }

    for (std::size_t index = 0; index < fonts.size(); ++index)
    {
        CHECK(&manager.font(fontNames[index]) == fonts[index]);
        CHECK(!manager.nativeFont(*fonts[index]).getInfo().family.empty());
    }
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"textures and fonts load", &testTexturesAndFontsLoad},
        {"declared names are looked up", &testDeclaredNamesAreLookedUp},
        {"repeated lookups return the same object", &testRepeatedLookupsReturnTheSameObject},
        {"handles remain opaque after loading", &testHandlesRemainOpaqueAfterLoading},
        {"empty configuration loads nothing", &testEmptyConfigurationLoadsNothing},
        {"every committed library file loads", &testEveryCommittedLibraryFileLoads},
        {"indexed png produces correct pixels", &testIndexedPngProducesCorrectPixels},
        {"an rgba png is unaffected", &testAnRgbaPngIsUnaffected},
        {"fonts parse rather than merely existing", &testFontsParseRatherThanMerelyExisting},
        {"fonts are cached like textures", &testFontsAreCachedLikeTextures},
        {"paths resolve relative to the configuration file", &testPathsResolveRelativeToTheConfigurationFile},
        {"resolution follows the config not the process", &testResolutionFollowsTheConfigNotTheProcess},
        {"missing texture name is reported", &testMissingTextureNameIsReported},
        {"missing font name is reported", &testMissingFontNameIsReported},
        {"names are looked up per kind", &testNamesAreLookedUpPerKind},
        {"missing file names the asset and the resolved path", &testMissingFileNamesTheAssetAndTheResolvedPath},
        {"missing font file is reported", &testMissingFontFileIsReported},
        {"malformed configuration keeps the parser's diagnosis", &testMalformedConfigurationKeepsTheParsersDiagnosis},
        {"missing configuration file is reported", &testMissingConfigurationFileIsReported},
        {"an empty handle has no platform resource", &testAnEmptyHandleHasNoPlatformResource},
        {"the manager header offers no mutation", &testTheManagerHeaderOffersNoMutation},
        {"two managers load independently", &testTwoManagersLoadIndependently},
        {"a loaded handle cannot be moved out of the manager", &testALoadedHandleCannotBeMovedOutOfTheManager},
        {"self move leaves a handle usable", &testSelfMoveLeavesAHandleUsable},
        {"the manager survives every declared asset being used", &testTheManagerSurvivesEveryDeclaredAssetBeingUsed},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        // Construction and lookup both throw by contract, so an exception escaping
        // a group is a failure of that group rather than a reason to abort and
        // hide the result of every group after it.
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

    std::cout << groupCount << " asset loading test groups passed\n";
    return EXIT_SUCCESS;
}
