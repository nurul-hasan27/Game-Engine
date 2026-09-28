#include "engine/Color.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/ecs/EntityView.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Text.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/graphics/SfmlRenderer.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/systems/RenderSystem.hpp"

#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Texture.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// The committed font names, as the asset configuration declares them.
//
// Hard-coded on purpose. Every real-render group here draws with a real font, and
// a test that read the name out of `assets.txt` would keep passing if the name
// were renamed away - it would just quietly test a different font. Pinning the
// strings means a rename breaks these tests loudly, which is what should happen.
// ---------------------------------------------------------------------------
constexpr std::string_view kFontNumbers = "fonts_numbers";
constexpr std::string_view kFontPixeled = "fonts_pixeled";
constexpr std::string_view kFontTech = "fonts_tech";

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

void checkNear(const float actual, const float expected, const float tolerance, const char* const expression,
               const char* const file, const int line)
{
    if (std::fabs(actual - expected) > tolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << " +/- " << tolerance << '\n';
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, tolerance) \
    checkNear((actual), (expected), (tolerance), #actual " ~= " #expected, __FILE__, __LINE__)

using engine::Color;
using engine::Vec2;
using engine::assets::Animation;
using engine::assets::AssetManager;
using engine::assets::AssetNotFoundError;
using engine::assets::Font;
using engine::assets::Texture;
using engine::components::Text;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::EntityView;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::input::ActionState;
using engine::systems::RenderSystem;

// ---------------------------------------------------------------------------
// Compile-time guarantees
//
// The shape of the new type, checked where only the compiler can. These hold
// regardless of runtime behaviour, so no mutation can make them pass by accident.
// ---------------------------------------------------------------------------

// A component is plain data. `is_aggregate` is what proves no constructor is
// hiding a defaulted field, and the copy/move/destroy properties are what prove
// it is an ordinary ECS value that a component storage can hold and hand out.
static_assert(std::is_aggregate_v<Text>, "Text must be a plain aggregate");
static_assert(std::is_default_constructible_v<Text>, "Text must be default constructible");
static_assert(std::is_copy_constructible_v<Text>, "Text must be copyable");
static_assert(std::is_copy_assignable_v<Text>, "Text must be copy assignable");
static_assert(std::is_move_constructible_v<Text>, "Text must be move constructible");
static_assert(std::is_move_assignable_v<Text>, "Text must be move assignable");
// Not trivially destructible, because a `std::string` is not - what matters
// is that nothing here owns a *resource*: no font handle, no pointer, no
// shared state. A copy is checked independent at runtime below.
static_assert(!std::has_virtual_destructor_v<Text>, "Text must be data with no virtual behaviour");

// No resource ownership and no back pointer. A component holds values; anything
// owning a resource or naming a manager would put a lifetime inside the ECS.
static_assert(!std::is_polymorphic_v<Text>, "Text must be data, not an interface");
static_assert(std::is_standard_layout_v<Text>, "Text must remain standard layout");

// The default character size is a documented choice, so it is pinned here as well
// as at runtime. 24 is what a 1280x720 window wants for a label; nothing in the
// course states a size, and the header says so.
static_assert(sizeof(decltype(Text::characterSize)) == sizeof(std::uint32_t),
              "characterSize must be a whole number of points, not a float");
static_assert(decltype(Text::characterSize){24U} == 24U, "the default character size must be 24");

// The renderer interface is SFML-free, so a gameplay translation unit can include
// it with no graphics library in reach. Proved structurally: the types the text
// draw takes must all be this engine's or the standard library's.
static_assert(!std::is_same_v<Renderer*, void>, "Renderer must remain an interface");
static_assert(std::is_abstract_v<Renderer>, "Renderer must be abstract");

// ---------------------------------------------------------------------------
// A recording renderer and a double asset manager
//
// The double records what a system submitted, by address for the handles and by
// value for everything else - the same split `RenderTest` uses, and for the same
// reason: a handle is non-copyable by design and its identity is the thing worth
// asserting.
// ---------------------------------------------------------------------------

struct TextDrawCall
{
    const Font* font = nullptr;
    std::string content;
    std::uint32_t characterSize = 0;
    Color color{};
    RenderTransform placement;
};

struct TextureDrawCall
{
    const Texture* texture = nullptr;
    RenderTransform placement;
};

class RecordingRenderer final : public Renderer
{
public:
    void beginFrame() override
    {
        ++m_beginFrames;
        m_order.clear();
    }

    void clear(const Color& color) override
    {
        m_clearColor = color;
        ++m_clears;
    }

    void drawRectangle(const Vec2&, const Color&, const RenderTransform&) override
    {
        m_order.emplace_back("rectangle");
    }

    void drawTexture(const Texture& texture, const RenderTransform& placement, const std::optional<engine::IntRect>&) override
    {
        m_textureDraws.push_back(TextureDrawCall{&texture, placement});
        m_order.emplace_back("texture");
    }

    void drawText(const Font& font, const std::string& content, const std::uint32_t characterSize,
                  const Color& color, const RenderTransform& placement) override
    {
        m_textDraws.push_back(TextDrawCall{&font, content, characterSize, color, placement});
        m_order.emplace_back("text");
    }

    void endFrame() override { ++m_endFrames; }
    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_endFrames; }

    [[nodiscard]] const std::vector<TextDrawCall>& textDraws() const noexcept { return m_textDraws; }
    [[nodiscard]] const std::vector<TextureDrawCall>& textureDraws() const noexcept { return m_textureDraws; }
    [[nodiscard]] const std::vector<std::string>& order() const noexcept { return m_order; }
    [[nodiscard]] std::size_t clearCount() const noexcept { return m_clears; }

private:
    std::vector<TextDrawCall> m_textDraws;
    std::vector<TextureDrawCall> m_textureDraws;
    std::vector<std::string> m_order;
    Color m_clearColor{};
    std::size_t m_clears = 0;
    std::size_t m_beginFrames = 0;
    std::size_t m_endFrames = 0;
};

/// A double that declares two fonts and two textures and records what was asked for.
class DoubleAssetManager final : public AssetManager
{
public:
    /// The handles this double hands back. Stable addresses, so a test can compare
    /// the pointer a system submitted against the one it should have.
    Font numbersFont;
    Font pixeledFont;
    Font techFont;
    Texture tileTexture;

    DoubleAssetManager()
    {
        m_animationNames.push_back("some_animation");
    }

    /// Records every font name asked for, in order.
    [[nodiscard]] const std::vector<std::string>& fontLookups() const noexcept { return m_fontLookups; }
    [[nodiscard]] std::size_t textureLookups() const noexcept { return m_textureLookups; }

    const Texture& texture(const std::string_view name) const override
    {
        ++m_textureLookups;
        if (name != "a_tile")
        {
            throw AssetNotFoundError{"no texture named '" + std::string{name} + "'"};
        }

        return tileTexture;
    }

    const engine::assets::Font& font(const std::string_view name) const override
    {
        m_fontLookups.emplace_back(name);

        if (name == kFontNumbers)
        {
            return numbersFont;
        }

        if (name == kFontPixeled)
        {
            return pixeledFont;
        }

        if (name == kFontTech)
        {
            return techFont;
        }

        throw AssetNotFoundError{"no font named '" + std::string{name} + "'"};
    }

    const Animation& animation(const std::string_view name) const override
    {
        if (name != "some_animation")
        {
            throw AssetNotFoundError{"no animation named '" + std::string{name} + "'"};
        }

        return m_animation;
    }

private:
    Animation m_animation{"a_tile", 1U, 1U, 8, 8};
    std::vector<std::string> m_animationNames;
    mutable std::vector<std::string> m_fontLookups;
    mutable std::size_t m_textureLookups = 0;
};

// ---------------------------------------------------------------------------
// Entity helpers
// ---------------------------------------------------------------------------

Entity& addTextEntity(EntityManager& world, std::string_view content, std::string_view fontName,
                      const std::uint32_t characterSize, const Vec2 position = Vec2{10.0F, 20.0F})
{
    Entity& entity = world.addEntity("label");
    entity.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    Text text;
    text.content = std::string{content};
    text.fontAssetName = std::string{fontName};
    text.characterSize = characterSize;
    entity.addComponent<Text>(text);

    return entity;
}

/// A camera that maps world space to screen space one to one, so a world
/// coordinate and the screen pixel it lands on are the same number and an
/// assertion about one is an assertion about the other.
[[nodiscard]] Camera identityCamera() { return {}; }

// ---------------------------------------------------------------------------
// Real-render measurement
//
// Everything here draws through the real `SfmlRenderer`, with real fonts loaded
// by the real `SfmlAssetManager`, into a real window, and measures the pixels.
//
// The invariant chosen is the **bounding box of the changed pixels**, not a
// comparison against a stored matrix. A glyph rasterised by a font library is not
// something to hard-code: hinting, antialiasing and the exact outline all vary by
// platform and version, and a matrix assertion would break on a machine that is
// not this one while saying nothing about correctness. A bounding box is stable,
// meaningful, and the thing the contracts below are actually about.
// ---------------------------------------------------------------------------

struct ChangedPixels
{
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;
    long long count = 0;

    [[nodiscard]] bool anythingChanged() const noexcept { return maxX >= minX; }
    [[nodiscard]] int width() const noexcept { return maxX - minX + 1; }
    [[nodiscard]] int height() const noexcept { return maxY - minY + 1; }

    /// Where the box sits, in half pixels, so a centred string lands on the
    /// integer just below the position and an odd-width one on the integer above.
    [[nodiscard]] float centreX() const noexcept { return (static_cast<float>(minX) + static_cast<float>(maxX)) / 2.0F; }
    [[nodiscard]] float centreY() const noexcept { return (static_cast<float>(minY) + static_cast<float>(maxY)) / 2.0F; }
};

/// Reads the window back and finds every pixel that is not `background`.
///
/// Must be called **before** `endFrame()`: on this platform the front buffer after
/// `display()` reads back black, while the buffer just drawn reads back correctly.
/// `RenderTest` documents the same constraint, and it is the reason every group
/// here measures inside the frame.
[[nodiscard]] ChangedPixels measureChanged(sf::RenderWindow& window, const sf::Color& background)
{
    ChangedPixels result;

    sf::Texture readback;
    if (!readback.create(window.getSize().x, window.getSize().y))
    {
        return result;
    }

    readback.update(window);
    const sf::Image image = readback.copyToImage();

    for (unsigned y = 0U; y < image.getSize().y; ++y)
    {
        for (unsigned x = 0U; x < image.getSize().x; ++x)
        {
            if (image.getPixel(x, y) == background)
            {
                continue;
            }

            if (result.maxX < result.minX)
            {
                result.minX = static_cast<int>(x);
                result.maxX = static_cast<int>(x);
                result.minY = static_cast<int>(y);
                result.maxY = static_cast<int>(y);
            }
            else
            {
                result.minX = std::min(result.minX, static_cast<int>(x));
                result.maxX = std::max(result.maxX, static_cast<int>(x));
                result.minY = std::min(result.minY, static_cast<int>(y));
                result.maxY = std::max(result.maxY, static_cast<int>(y));
            }

            ++result.count;
        }
    }

    return result;
}

/// A window plus a real renderer plus a real asset manager, for the pixel groups.
///
/// The window is hidden: a test that needs to see nothing does not want a dozen
/// windows appearing. SFML still creates a real GL context, which is what makes
/// the readback real rather than a simulation.
class RealRenderFixture
{
public:
    RealRenderFixture()
        : m_window(sf::VideoMode{400U, 200U}, "text rendering"),
          m_renderer{m_window},
          m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        m_window.setVisible(false);
    }

    [[nodiscard]] engine::graphics::SfmlRenderer& renderer() noexcept { return m_renderer; }
    [[nodiscard]] const engine::assets::AssetManager& assets() const noexcept { return m_assets; }
    [[nodiscard]] sf::RenderWindow& window() noexcept { return m_window; }

    /// Clears, draws one string, and measures what changed.
    ///
    /// The whole "draw and measure" sequence in one place, so every group does it
    /// the same way and none of them can forget that the readback belongs inside
    /// the frame.
    [[nodiscard]] ChangedPixels measureText(const Font& font, const std::string& content,
                                            const std::uint32_t characterSize, const RenderTransform& placement,
                                            const Color& color = engine::kWhite)
    {
        m_renderer.beginFrame();
        m_renderer.clear(engine::kBlack);
        m_renderer.drawText(font, content, characterSize, color, placement);

        const ChangedPixels result = measureChanged(m_window, background());

        m_renderer.endFrame();
        return result;
    }

    /// Same, with a background of a known colour, for the colour groups.
    [[nodiscard]] ChangedPixels measureTextOn(const sf::Color& background, const Font& font, const std::string& content,
                                              const std::uint32_t characterSize, const Color& color,
                                              const RenderTransform& placement)
    {
        m_renderer.beginFrame();
        m_renderer.clear(Color{static_cast<float>(background.r) / 255.0F, static_cast<float>(background.g) / 255.0F,
                               static_cast<float>(background.b) / 255.0F, 1.0F});
        m_renderer.drawText(font, content, characterSize, color, placement);

        const ChangedPixels result = measureChanged(m_window, background);

        m_renderer.endFrame();
        return result;
    }

    /// The brightest pixel drawn, which identifies the fill colour actually used.
    [[nodiscard]] sf::Color brightestDrawn(const sf::Color& background)
    {
        sf::Texture readback;
        if (!readback.create(m_window.getSize().x, m_window.getSize().y))
        {
            return sf::Color::Black;
        }

        m_renderer.beginFrame();
        m_renderer.clear(engine::kBlack);
        readback.update(m_window);

        const sf::Image image = readback.copyToImage();
        int best = 0;
        sf::Color brightest{0, 0, 0, 255};

        for (unsigned y = 0U; y < image.getSize().y; ++y)
        {
            for (unsigned x = 0U; x < image.getSize().x; ++x)
            {
                const sf::Color pixel = image.getPixel(x, y);
                if (pixel == background)
                {
                    continue;
                }

                const int total = static_cast<int>(pixel.r) + static_cast<int>(pixel.g) + static_cast<int>(pixel.b);
                if (total > best)
                {
                    best = total;
                    brightest = pixel;
                }
            }
        }

        m_renderer.endFrame();
        return brightest;
    }

    /// The background every measurement is compared against.
    ///
    /// A function rather than a `constexpr` member because `sf::Color` is not a
    /// literal type in this library, so it cannot be a `constexpr` variable.
    [[nodiscard]] static sf::Color background() noexcept { return sf::Color{0, 0, 0, 255}; }

private:
    sf::RenderWindow m_window;
    engine::graphics::SfmlRenderer m_renderer;
    engine::assets::SfmlAssetManager m_assets;
};

// ---------------------------------------------------------------------------
// Source scanning
// ---------------------------------------------------------------------------

/// Strips `//` comments, leaving only code.
///
/// The new headers *discuss* the SFML boundary in prose, and the sentence saying
/// "this header mentions no `sf::` type" contains the characters a substring search
/// looks for. Scanning raw text therefore reports these headers as violations of
/// the rule they were written to state - the same false positive this repository
/// has hit in three earlier phases. The rule is about code, so the comments go
/// first.
[[nodiscard]] std::string codeOf(const char* const path)
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

    std::istringstream lines{buffer.str()};
    std::string code;
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

[[nodiscard]] bool has(const std::string& haystack, const std::string_view needle) noexcept
{
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::size_t countOf(const std::string& haystack, const std::string_view needle) noexcept
{
    std::size_t total = 0U;
    for (std::string::size_type at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
    {
        ++total;
    }

    return total;
}

// ===========================================================================
// A. The Text component
// ===========================================================================

void testTextDefaults()
{
    const Text text;

    // A default-constructed label is empty and names no font. Both are honest
    // states rather than errors, and neither draws anything: an empty string is
    // measured at 0x0 by every committed font.
    CHECK(text.content.empty());
    CHECK(text.fontAssetName.empty());
    CHECK(text.characterSize == 24U);
}

void testTextIsAnAggregate()
{
    // Brace-initialised, in declaration order, the way every other component in
    // this engine is built. If a field were added between these three, or a
    // constructor appeared, this would stop compiling - which is the point.
    const Text text{"HELLO", std::string{kFontPixeled}, 18U};

    CHECK(text.content == "HELLO");
    CHECK(text.fontAssetName == kFontPixeled);
    CHECK(text.characterSize == 18U);
}

void testTextIsCopyableAndIndependent()
{
    Text original{"SPAWN 3,4", std::string{kFontTech}, 12U};
    const Text copy = original;

    CHECK(copy.content == original.content);
    CHECK(copy.fontAssetName == original.fontAssetName);
    CHECK(copy.characterSize == original.characterSize);

    // And the copy is a value: changing it must not reach back.
    Text mutableCopy{original};
    mutableCopy.content = "CHANGED";
    mutableCopy.characterSize = 99U;
    CHECK(original.content == "SPAWN 3,4");
    CHECK(original.characterSize == 12U);
}

void testTextIsStoredOnEntities()
{
    EntityManager world;
    addTextEntity(world, "LEVEL 1", kFontPixeled, 24U, Vec2{64.0F, 100.0F});

    EntityView labels = world.getEntities("label");
    std::size_t found = 0U;
    for (auto&& entity : labels)
    {
        ++found;
        CHECK(entity.hasComponent<Text>());
        CHECK(entity.hasComponent<Transform>());

        const Text& text = entity.getComponent<Text>();
        CHECK(text.content == "LEVEL 1");
        CHECK(text.fontAssetName == kFontPixeled);
        CHECK(text.characterSize == 24U);
    }

    CHECK(found == 1U);
}

void testTextCarriesNoColourAndNoAnchor()
{
    // A whole-type check rather than a blacklist: these are the only three fields
    // the component is allowed to have, so any fourth is a new decision someone
    // made rather than one this test happened not to forbid.
    const std::string code = codeOf(ENGINE_TEXT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // The three fields, and only the three fields.
    CHECK(has(code, "std::string content;"));
    CHECK(has(code, "std::string fontAssetName;"));
    CHECK(has(code, "std::uint32_t characterSize"));

    CHECK(countOf(code, "std::string ") == 2U);
    CHECK(countOf(code, "std::uint32_t ") == 1U);

    // No colour, because `Texture` and `Animation` carry none and text draws from
    // a font rather than being a coloured primitive. No alignment, no origin, no
    // position, no scale, because `Transform` and the renderer's centre convention
    // already answer those and a second answer could disagree with the first.
    const std::string_view forbidden[] = {"Color", "color", "sf::", "SFML", "align", "Align", "origin", "Origin",
                                          "position", "Position", "scale", "Scale", "Font*", "Font&", "Font "};
    for (const std::string_view needle : forbidden)
    {
        if (has(code, needle))
        {
            std::cerr << "    Text.hpp declares something it should not: " << needle << '\n';
        }

        CHECK_FALSE(has(code, needle));
    }
}

// ===========================================================================
// B. The Renderer API
// ===========================================================================

void testTheRendererInterfaceHasATextDraw()
{
    // Stated as a type rather than called: the point is that the signature is part
    // of the interface, so a caller can be written against it and implemented
    // without a graphics library in reach.
    using DrawText = void (Renderer::*)(const Font&, const std::string&, std::uint32_t, const Color&,
                                        const RenderTransform&);

    const DrawText drawText = &Renderer::drawText;
    CHECK(drawText != nullptr);

    // Every parameter is a type this engine owns or the standard library owns. A
    // graphics library's text type could not be named here at all, which is the
    // reason the pieces arrive separately.
    // `decltype` without `&` on a *virtual* member yields the function type, not a
    // pointer to it, and then "call to a non-static member function" is what the
    // compiler says. `&` asks for the pointer-to-member, which is the shape the
    // interface really has and the shape a caller binds to.
    static_assert(std::is_same_v<decltype(&Renderer::drawText),
                                 void (Renderer::*)(const Font&, const std::string&, std::uint32_t, const Color&,
                                                    const RenderTransform&)>,
                  "the text draw must take only engine and standard library types");
}

void testTheRendererHeaderIsSfmlFree()
{
    // Comments stripped first: this header's own documentation names the graphics
    // type it is written to keep out, so a raw scan would match its explanation.
    const std::string code = codeOf(ENGINE_RENDERER_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK_FALSE(has(code, "sf::"));
    CHECK_FALSE(has(code, "<SFML"));
    CHECK_FALSE(has(code, "SFML/"));
}

void testTheRendererHeaderNamesNoTextObject()
{
    // Specifically: a graphics library's text type must not appear, by any name.
    // This is the exact leak the interface exists to prevent, so it is checked by
    // name rather than by the generic `sf::` search alone.
    const std::string code = codeOf(ENGINE_RENDERER_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    const std::vector<std::string> forbidden{"sf::Text", "Text&", "Text *", "Text*", "class Text", "struct Text"};
    for (const std::string& needle : forbidden)
    {
        CHECK_FALSE(has(code, needle));
    }

    // And the engine's own `Text` is reached through the render *system*, not
    // through this header - a component has no business in a drawing interface.
    CHECK_FALSE(has(code, "components/Text.hpp"));
}

void testTheExistingRendererMethodsAreUnchanged()
{
    // The three methods that existed before, with the signatures they had. If a
    // new draw operation changed one of them, every existing test in the project
    // would stop compiling - but this says so at the point of the change rather
    // than leaving it to be noticed.
    using DrawRectangle = void (Renderer::*)(const Vec2&, const Color&, const RenderTransform&);
    using DrawTexture = void (Renderer::*)(const Texture&, const RenderTransform&, const std::optional<engine::IntRect>&);

    const DrawRectangle drawRectangle = &Renderer::drawRectangle;
    const DrawTexture drawTexture = &Renderer::drawTexture;

    CHECK(drawRectangle != nullptr);
    CHECK(drawTexture != nullptr);
    // Also a virtual member, so the same `&` the others need.
    CHECK(&Renderer::frameCount != nullptr);
}

// ===========================================================================
// C. Font lookup
// ===========================================================================

void testAMissingFontNameStillThrows()
{
    // The existing asset-level rule, unchanged: a name that is not declared
    // throws `AssetNotFoundError`, and a system that swallows it would produce a
    // label that silently never appears.
    DoubleAssetManager assets;

    bool threw = false;
    std::string message;
    try
    {
        static_cast<void>(assets.font("no_such_font"));
    }
    catch (const AssetNotFoundError& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    CHECK(has(message, "no_such_font"));
}

void testTheRealManagerResolvesEveryCommittedFont()
{
    // Through the real manager and the real committed files, so this is a claim
    // about the shipped asset configuration rather than about a double.
    engine::assets::SfmlAssetManager assets{std::filesystem::path{engine::config::kAssetsConfig}};

    const Font& numbers = assets.font(kFontNumbers);
    const Font& pixeled = assets.font(kFontPixeled);
    const Font& tech = assets.font(kFontTech);

    // Three names, three resources. The doubles are empty handles and compare
    // nothing, so identity is asserted through the addresses being distinct - a
    // cache that returned the same handle for two names would collapse them.
    CHECK(&numbers != &pixeled);
    CHECK(&pixeled != &tech);
    CHECK(&numbers != &tech);

    // And asking again gives the same one, which is what "cached" means.
    CHECK(&assets.font(kFontPixeled) == &pixeled);
}

void testARealFontHandleCanBeResolvedAndDrawn()
{
    // The whole chain once, before any pixel assertion: config file to loaded font
    // to a resolved handle. If this failed, every pixel group below would fail for
    // a reason that had nothing to do with text rendering.
    engine::assets::SfmlAssetManager assets{std::filesystem::path{engine::config::kAssetsConfig}};
    const Font& font = assets.font(kFontPixeled);

    RealRenderFixture fixture;
    const ChangedPixels drawn = fixture.measureText(font, "OK", 24U, RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(drawn.anythingChanged());
}

// ===========================================================================
// D. RenderSystem
// ===========================================================================

void testTheTextQueryDrawsEntitiesWithText()
{
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "SCORE 100", kFontPixeled, 24U);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.textDraws().size() == 1U);
    if (!renderer.textDraws().empty())
    {
        CHECK(renderer.textDraws().front().content == "SCORE 100");
    }
}

void testTheTextQueryPassesTheComponentCharacterSizeThrough()
{
    // A **non-default** size, on purpose.
    //
    // Asserting the size was 24 proved nothing about pass-through, because 24 is
    // also the component's default *and* the value a plausible mistake would
    // hard-code. A mutation that ignored the component and always submitted 24
    // passed every group in this file. 7 is nothing to do with either number, so it
    // can only survive if the component's field is genuinely being read.
    //
    // Two entities at two deliberately different sizes, and each keeps its own.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "SMALL", kFontPixeled, 7U, Vec2{10.0F, 10.0F});
    addTextEntity(world, "LARGE", kFontPixeled, 64U, Vec2{80.0F, 80.0F});

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.textDraws().size() == 2U);
    if (renderer.textDraws().size() == 2U)
    {
        CHECK(renderer.textDraws()[0].characterSize == 7U);
        CHECK(renderer.textDraws()[1].characterSize == 64U);
    }
}

void testTheTextQueryResolvesTheFontByName()
{
    // By identity, not by value: a handle is non-copyable by design, and the thing
    // worth asserting is that the system submitted the asset the name resolved to
    // rather than a copy of it.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "A", kFontNumbers, 12U);
    addTextEntity(world, "B", kFontTech, 24U);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.textDraws().size() == 2U);

    // The lookup order follows creation order, and each name went to the right font.
    CHECK(assets.fontLookups().size() == 2U);
    if (assets.fontLookups().size() == 2U)
    {
        CHECK(assets.fontLookups()[0] == kFontNumbers);
        CHECK(assets.fontLookups()[1] == kFontTech);
    }

    std::vector<const Font*> submitted;
    for (const TextDrawCall& call : renderer.textDraws())
    {
        submitted.push_back(call.font);
    }

    // Exactly the two handles the double owns, once each.
    std::size_t foundNumbers = 0U;
    std::size_t foundTech = 0U;
    for (const Font* font : submitted)
    {
        foundNumbers += (font == &assets.numbersFont) ? 1U : 0U;
        foundTech += (font == &assets.techFont) ? 1U : 0U;
    }

    CHECK(foundNumbers == 1U);
    CHECK(foundTech == 1U);
}

void testAMissingFontPropagatesOutOfTheTextQuery()
{
    // Deliberately not caught by the system. A label that silently draws nothing
    // is a bug that surfaces much later as missing text with nothing pointing at
    // the cause, so the lookup's exception travels all the way out.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "GHOST", "no_such_font", 24U);

    const ActionState actions;

    bool threw = false;
    std::string message;
    try
    {
        system.update(world, actions, 0.0F);
    }
    catch (const AssetNotFoundError& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    CHECK(has(message, "no_such_font"));
}

void testTheTextQueryReadsTheTransformAndNeverWritesIt()
{
    // The Transform is a world position however the camera is configured, and a
    // render pass that wrote to it would corrupt the simulation's state. The
    // rectangle and texture queries make the same promise, and text keeps it.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    Entity& entity = addTextEntity(world, "FIXED", kFontPixeled, 24U, Vec2{321.0F, 654.0F});

    const Vec2 before = entity.getComponent<Transform>().position;
    const Vec2 velocityBefore = entity.getComponent<Transform>().velocity;

    const ActionState actions;
    system.update(world, actions, 0.0F);

    const Transform& after = entity.getComponent<Transform>();
    CHECK_NEAR(after.position.x, before.x, 0.0F);
    CHECK_NEAR(after.position.y, before.y, 0.0F);
    CHECK_NEAR(after.velocity.x, velocityBefore.x, 0.0F);
    CHECK_NEAR(after.velocity.y, velocityBefore.y, 0.0F);
}

void testTheTextQueryAppliesTheCameraLikeEveryOtherQuery()
{
    // Text is world space in this engine, so it goes through the same
    // `toRenderTransform` a sprite does. A camera that maps world to something
    // else has to move the label by exactly as much as it moves a texture, or the
    // two would drift apart on screen.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "T", kFontPixeled, 24U, Vec2{700.0F, 500.0F});
    addTextEntity(world, "S", kFontPixeled, 24U, Vec2{700.0F, 500.0F});

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.textDraws().size() == 2U);
    if (renderer.textDraws().size() == 2U)
    {
        // The identity camera maps world to screen one to one.
        CHECK_NEAR(renderer.textDraws()[0].placement.position.x, 700.0F, 0.001F);
        CHECK_NEAR(renderer.textDraws()[0].placement.position.y, 500.0F, 0.001F);
    }

    // A camera with a zoom must scale the label's placement, because the
    // placement carries the zoom and the renderer's states function applies it.
    RecordingRenderer zoomedRenderer;
    Camera zoomed;
    zoomed.setZoom(2.0F);
    RenderSystem zoomedSystem{zoomedRenderer, zoomed, assets};

    EntityManager zoomedWorld;
    addTextEntity(zoomedWorld, "T", kFontPixeled, 24U, Vec2{700.0F, 500.0F});
    zoomedSystem.update(zoomedWorld, actions, 0.0F);

    CHECK(zoomedRenderer.textDraws().size() == 1U);
    if (!zoomedRenderer.textDraws().empty())
    {
        // Scale is the one the camera multiplies in, and a caller can see it.
        CHECK(zoomedRenderer.textDraws().front().placement.scale.x > 1.0F);
    }
}

void testTheTextQueryDrawsLastOfAllFourQueries()
{
    // The order is a consequence of query 4 being appended, not a layering
    // decision - there is no z-order anywhere in this engine. Asserted because
    // "a label draws on top of a sprite" is a visible property a later phase would
    // otherwise have to take on faith.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;

    Entity& rectangle = world.addEntity("box");
    rectangle.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<engine::components::Rectangle>();

    Entity& sprite = world.addEntity("sprite");
    sprite.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    sprite.addComponent<engine::components::Texture>(engine::components::Texture{"a_tile"});

    addTextEntity(world, "ON TOP", kFontPixeled, 24U);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    const std::vector<std::string>& order = renderer.order();
    CHECK(order.size() == 3U);
    if (order.size() == 3U)
    {
        CHECK(order[0] == "rectangle");
        CHECK(order[1] == "texture");
        CHECK(order[2] == "text");
    }
}

void testAStringIsDrawnAlongsideSomethingElseOnTheSameEntity()
{
    // No skip rule, and deliberately. Query 2 skips an entity that animates
    // because a texture and an animation would draw *the same artwork twice*. A
    // string and a rectangle draw different things - a label and a box behind it -
    // so drawing both is what was asked for, and the query order above already
    // decides which is on top.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    Entity& entity = world.addEntity("label_on_box");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<engine::components::Rectangle>();

    Text text;
    text.content = "LABEL";
    text.fontAssetName = kFontPixeled;
    entity.addComponent<Text>(text);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    // Both, and the rectangle first so the string is on top of it.
    const std::vector<std::string>& order = renderer.order();
    CHECK(order.size() == 2U);
    if (order.size() == 2U)
    {
        CHECK(order[0] == "rectangle");
        CHECK(order[1] == "text");
    }

    CHECK(renderer.textDraws().size() == 1U);
}

void testTheTextQueryIgnoresEntitiesMissingAComponent()
{
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;

    // Text with no Transform: no placement, so nothing to draw.
    Entity& noTransform = world.addEntity("orphan_text");
    Text orphan;
    orphan.content = "NOWHERE";
    orphan.fontAssetName = kFontPixeled;
    noTransform.addComponent<Text>(orphan);

    // A Transform with no Text: a plain entity.
    Entity& noText = world.addEntity("plain");
    noText.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});

    // A dead entity with both: must not draw. Death goes through the manager -
    // `Entity::markDead` is private for exactly this reason - and `destroyEntity`
    // takes effect immediately, so the entity has already stopped appearing in
    // views by the time the system runs.
    Entity& dead = addTextEntity(world, "DEAD", kFontPixeled, 24U);
    world.destroyEntity(dead);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    // And nothing was looked up for an entity that was never drawn.
    CHECK(renderer.textDraws().empty());
    CHECK(assets.fontLookups().empty());
}

void testAnEmptyStringIsSubmittedAndTheRendererDecidesWhatItMeans()
{
    // The component is data and the renderer is behaviour, so the system hands an
    // empty string straight through. Whether an empty string draws is a rendering
    // question, and the real-render groups answer it; asserting here that the
    // system *rejects* it would be inventing a rule the course does not state.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    addTextEntity(world, "", kFontPixeled, 24U);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.textDraws().size() == 1U);
    if (!renderer.textDraws().empty())
    {
        CHECK(renderer.textDraws().front().content.empty());
    }
}

// ===========================================================================
// E. Real rendering: a real window, a real font, real pixels
// ===========================================================================

void testTextChangesPixels()
{
    // The core claim. Everything else in this file assumes it.
    RealRenderFixture fixture;
    const ChangedPixels drawn = fixture.measureText(fixture.assets().font(kFontPixeled), "HELLO", 24U,
                                                    RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(drawn.anythingChanged());
    CHECK(drawn.count > 0);

    // A string is wider than it is tall, and taller than a few pixels, at 24
    // points in a pixel font. Loose on purpose: a glyph outline is not a
    // contract, but "roughly landscape, definitely not a speck" is.
    CHECK(drawn.width() > drawn.height());
    CHECK(drawn.width() > 20);
    CHECK(drawn.height() > 8);
}

void testTextIsCentredOnItsPosition()
{
    // ### The convention, pinned
    //
    // Position is the **centre** of what is drawn, for text exactly as it is for a
    // rectangle and a sprite - Lecture 11 section 5, and the reason
    // `drawRectangle` sets its origin to `size / 2`.
    //
    // The tolerance is one pixel because of where the box *centre* lands, not
    // because the convention is loose: a box of odd width centred on an integer
    // position has its centre half a pixel to one side, so the measured centre is
    // `position - 0.5`. One pixel covers that and nothing more - a box drawn from
    // its top-left corner instead would be off by half its own width, tens of
    // pixels here.
    //
    // Checked for all three committed fonts, and that is not decoration: measured
    // against `tech`, the origin cannot be `width / 2`, because `tech`'s glyphs
    // have a left side bearing and its reported bounds start at -2. Using `width/2`
    // would shift every `tech` string two pixels right, which is invisible in a
    // screenshot and caught by exactly this assertion.
    RealRenderFixture fixture;

    for (const std::string_view name : {kFontNumbers, kFontPixeled, kFontTech})
    {
        const ChangedPixels drawn = fixture.measureText(fixture.assets().font(name), "AB0", 24U,
                                                        RenderTransform{Vec2{200.0F, 100.0F}});

        if (!drawn.anythingChanged())
        {
            std::cerr << "    font " << name << " drew nothing\n";
        }

        CHECK(drawn.anythingChanged());

        if (drawn.anythingChanged())
        {
            CHECK_NEAR(drawn.centreX(), 200.0F, 1.0F);
            CHECK_NEAR(drawn.centreY(), 100.0F, 1.0F);
        }
    }
}

void testMovingThePositionMovesTheText()
{
    // Same pixels, same size, different place. Asserted as a *delta* rather than
    // as absolute boxes, because that is the property: the placement is what moved,
    // not the glyphs.
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);

    const ChangedPixels first = fixture.measureText(font, "X", 24U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels second = fixture.measureText(font, "X", 24U, RenderTransform{Vec2{80.0F, 40.0F}});
    const ChangedPixels third = fixture.measureText(font, "X", 24U, RenderTransform{Vec2{330.0F, 160.0F}});

    CHECK(first.anythingChanged());
    CHECK(second.anythingChanged());
    CHECK(third.anythingChanged());

    if (!(first.anythingChanged() && second.anythingChanged() && third.anythingChanged()))
    {
        return;
    }

    // The same string at the same size is the same picture.
    CHECK(first.width() == second.width());
    CHECK(first.height() == second.height());
    CHECK(first.count == second.count);
    CHECK(first.count == third.count);

    // And it moved by exactly the amount the position moved. (200,100) to (80,40)
    // is -120 in both axes; the box centre moves by the same -120.
    CHECK_NEAR(second.centreX() - first.centreX(), -120.0F, 1.0F);
    CHECK_NEAR(second.centreY() - first.centreY(), -60.0F, 1.0F);
    CHECK_NEAR(third.centreX() - first.centreX(), 130.0F, 1.0F);
    CHECK_NEAR(third.centreY() - first.centreY(), 60.0F, 1.0F);
}

void testCharacterSizeChangesHowBigTheTextIs()
{
    // `characterSize` is a **font request**, applied before rasterization, and this
    // is what separates it from `Transform::scale` below. Asking a font for a
    // bigger size makes the rasterizer choose bigger outlines.
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);

    const ChangedPixels small = fixture.measureText(font, "AB0", 12U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels medium = fixture.measureText(font, "AB0", 24U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels large = fixture.measureText(font, "AB0", 48U, RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(small.anythingChanged());
    CHECK(medium.anythingChanged());
    CHECK(large.anythingChanged());

    if (!(small.anythingChanged() && medium.anythingChanged() && large.anythingChanged()))
    {
        return;
    }

    CHECK(medium.width() > small.width());
    CHECK(large.width() > medium.width());
    CHECK(medium.height() > small.height());
    CHECK(large.height() > medium.height());

    // Doubling the size roughly doubles the width. "Roughly" because a font's
    // advances do not scale exactly linearly, and asserting exact doubling would be
    // asserting a property of the typeface rather than of the engine.
    const float ratio = static_cast<float>(large.width()) / static_cast<float>(small.width());
    CHECK(ratio > 3.0F);
    CHECK(ratio < 5.0F);
}

void testScaleEnlargesTheTextLikeAnyOtherRenderable()
{
    // `Transform::scale` is the geometric multiplier, applied after rasterization,
    // and it is the same field every other renderable reads. Enlarging with it
    // gives the same picture at a different size as enlarging by rasterizing at a
    // bigger size - the pixel *counts* are close but the glyph rasterization
    // differs, which is exactly why both fields exist.
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);

    const ChangedPixels one = fixture.measureText(font, "X", 24U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels two = fixture.measureText(font, "X", 24U,
                                                   RenderTransform{Vec2{200.0F, 100.0F}, Vec2{2.0F, 2.0F}, 0.0F});
    const ChangedPixels three = fixture.measureText(font, "X", 24U,
                                                     RenderTransform{Vec2{200.0F, 100.0F}, Vec2{3.0F, 3.0F}, 0.0F});

    CHECK(one.anythingChanged());
    CHECK(two.anythingChanged());
    CHECK(three.anythingChanged());

    if (!(one.anythingChanged() && two.anythingChanged() && three.anythingChanged()))
    {
        return;
    }

    CHECK(two.width() > one.width());
    CHECK(three.width() > two.width());
    CHECK(two.height() > one.height());
    CHECK(three.height() > two.height());

    // Still centred on the position after scaling, because the origin is set
    // before the transform is applied and the transform scales about it.
    CHECK_NEAR(two.centreX(), 200.0F, 1.5F);
    CHECK_NEAR(two.centreY(), 100.0F, 1.5F);
    CHECK_NEAR(three.centreX(), 200.0F, 2.0F);
    CHECK_NEAR(three.centreY(), 100.0F, 2.0F);
}

void testTheFontActuallyChangesTheOutput()
{
    // ### Proving the *selected* font was used, not merely that a font was
    //
    // A test that only checks "some pixels changed" passes just as well if the
    // implementation quietly substituted a default font for every request. This one
    // draws the **same string at the same size in all three committed fonts** and
    // measures all three, so a substituted font cannot produce three different
    // pictures.
    //
    // Measured on this repository: for "AB0" at 24 points the boxes are
    // 38x18 (numbers), 68x30 (pixeled) and 46x18 (tech), and the pixel counts are
    // 346, 1293 and 668. Those are recorded as *measurements*, not as
    // expectations: the assertion below is that the three differ from each other,
    // which is the contract, and it holds for any three genuinely different fonts.
    RealRenderFixture fixture;

    const ChangedPixels numbers = fixture.measureText(fixture.assets().font(kFontNumbers), "AB0", 24U,
                                                      RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels pixeled = fixture.measureText(fixture.assets().font(kFontPixeled), "AB0", 24U,
                                                      RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels tech = fixture.measureText(fixture.assets().font(kFontTech), "AB0", 24U,
                                                   RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(numbers.anythingChanged());
    CHECK(pixeled.anythingChanged());
    CHECK(tech.anythingChanged());

    if (!(numbers.anythingChanged() && pixeled.anythingChanged() && tech.anythingChanged()))
    {
        return;
    }

    // Three different widths. A substituted font would collapse these.
    CHECK(numbers.width() != pixeled.width());
    CHECK(pixeled.width() != tech.width());
    CHECK(numbers.width() != tech.width());

    // And three different pictures, not three similar boxes of the same glyphs.
    CHECK(numbers.count != pixeled.count);
    CHECK(pixeled.count != tech.count);
    CHECK(numbers.count != tech.count);

    // All three are still centred, so the difference is the font and not a
    // different anchor.
    CHECK_NEAR(numbers.centreX(), 200.0F, 1.0F);
    CHECK_NEAR(pixeled.centreX(), 200.0F, 1.0F);
    CHECK_NEAR(tech.centreX(), 200.0F, 1.0F);
}

void testTheContentChangesTheOutput()
{
    // The other half of "the draw call was used": a different string has to give a
    // different picture, or the implementation might be drawing a constant.
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);

    const ChangedPixels ab0 = fixture.measureText(font, "AB0", 24U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels hello = fixture.measureText(font, "HELLO", 24U, RenderTransform{Vec2{200.0F, 100.0F}});
    const ChangedPixels one = fixture.measureText(font, "I", 24U, RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(ab0.anythingChanged());
    CHECK(hello.anythingChanged());
    CHECK(one.anythingChanged());

    if (!(ab0.anythingChanged() && hello.anythingChanged() && one.anythingChanged()))
    {
        return;
    }

    CHECK(hello.width() > ab0.width());
    CHECK(one.width() < ab0.width());
    CHECK(one.count < ab0.count);
}

void testTheSpecificCharactersAreRenderedNotJustTheLength()
{
    // ### The gap this group closes
    //
    // `the content changes the output` compares three strings and asserts how their
    // widths relate. That is a real contract, but it is satisfied by *any* picture
    // whose size depends on the string's **length**: a mutation that replaced the
    // content with the same number of one repeated glyph kept every one of those
    // relationships true, and passed.
    //
    // So the comparison here is length-matched and glyph-different. "AB0" and "XXX"
    // are three characters each, and if the implementation is drawing the characters
    // it was given, they cannot be the same picture. If it is drawing a constant,
    // or a repeated glyph of the right count, they are - which is what makes this
    // the group that distinguishes "renders the string" from "renders something of
    // the right size".
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);
    const RenderTransform centre{Vec2{200.0F, 100.0F}, Vec2{1.0F, 1.0F}, 0.0F};

    const ChangedPixels ab0 = fixture.measureText(font, "AB0", 24U, centre);
    const ChangedPixels xxx = fixture.measureText(font, "XXX", 24U, centre);
    const ChangedPixels aaa = fixture.measureText(font, "AAA", 24U, centre);

    CHECK(ab0.anythingChanged());
    CHECK(xxx.anythingChanged());
    CHECK(aaa.anythingChanged());

    if (!(ab0.anythingChanged() && xxx.anythingChanged() && aaa.anythingChanged()))
    {
        return;
    }

    // Same length, different glyphs, different pictures. Not merely a different
    // bounding box - a different *pixel count*, which no amount of re-spacing of one
    // repeated glyph would produce.
    CHECK(ab0.count != xxx.count);
    CHECK(ab0.count != aaa.count);

    // The same argument with an even tighter pair: two runs of the same length built
    // from entirely different characters.
    const ChangedPixels iii = fixture.measureText(font, "III", 24U, centre);
    CHECK(iii.anythingChanged());
    if (iii.anythingChanged())
    {
        CHECK(iii.count != xxx.count);
    }

    // And a string of the *same* content drawn twice is identical, which is what
    // makes the comparisons above about the content and not about the frame.
    const ChangedPixels ab0Again = fixture.measureText(font, "AB0", 24U, centre);
    CHECK(ab0Again.count == ab0.count);
    CHECK(ab0Again.width() == ab0.width());
    CHECK(ab0Again.minX == ab0.minX);
    CHECK(ab0Again.minY == ab0.minY);
}

void testTheColourIsHonoured()
{
    // Exactly, not approximately: the fill colour is written straight into the
    // draw call, and the brightest pixel drawn is the fill.
    RealRenderFixture fixture;
    const Font& font = fixture.assets().font(kFontPixeled);

    for (const Color& asked : {engine::kWhite, Color{1.0F, 0.0F, 0.0F, 1.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}})
    {
        fixture.renderer().beginFrame();
        fixture.renderer().clear(engine::kBlack);
        fixture.renderer().drawText(font, "X", 24U, asked, RenderTransform{Vec2{200.0F, 100.0F}});

        sf::Texture readback;
        readback.create(fixture.window().getSize().x, fixture.window().getSize().y);
        readback.update(fixture.window());
        const sf::Image image = readback.copyToImage();

        int best = 0;
        sf::Color brightest{0, 0, 0, 255};
        for (unsigned y = 0U; y < image.getSize().y; ++y)
        {
            for (unsigned x = 0U; x < image.getSize().x; ++x)
            {
                const sf::Color pixel = image.getPixel(x, y);
                if (pixel == RealRenderFixture::background())
                {
                    continue;
                }

                const int total = static_cast<int>(pixel.r) + static_cast<int>(pixel.g) + static_cast<int>(pixel.b);
                if (total > best)
                {
                    best = total;
                    brightest = pixel;
                }
            }
        }

        fixture.renderer().endFrame();

        // The fill, to within the byte the channel conversion produces.
        CHECK_NEAR(static_cast<float>(brightest.r), asked.red * 255.0F, 1.0F);
        CHECK_NEAR(static_cast<float>(brightest.g), asked.green * 255.0F, 1.0F);
        CHECK_NEAR(static_cast<float>(brightest.b), asked.blue * 255.0F, 1.0F);
    }
}

void testAnEmptyStringDrawsNothing()
{
    // Measured, not assumed: an empty string measures 0x0 in all three committed
    // fonts, so there is nothing to place and nothing to draw. A label whose value
    // is nothing is not an error, and this says the renderer treats it that way
    // rather than throwing or drawing a stray glyph.
    RealRenderFixture fixture;

    for (const std::string_view name : {kFontNumbers, kFontPixeled, kFontTech})
    {
        const ChangedPixels drawn = fixture.measureText(fixture.assets().font(name), "", 24U,
                                                        RenderTransform{Vec2{200.0F, 100.0F}});

        if (drawn.anythingChanged())
        {
            std::cerr << "    font " << name << " drew something for an empty string\n";
        }

        CHECK_FALSE(drawn.anythingChanged());
        CHECK(drawn.count == 0);
    }
}

void testTextOnANonBlackBackgroundIsVisible()
{
    // On a coloured background, so "some pixels changed" cannot be satisfied by an
    // accidental white-on-white that happened to be equal to the background.
    RealRenderFixture fixture;
    const sf::Color background{16, 32, 48, 255};

    const ChangedPixels drawn = fixture.measureTextOn(background, fixture.assets().font(kFontPixeled), "VISIBLE", 24U,
                                                     engine::kWhite, RenderTransform{Vec2{200.0F, 100.0F}});

    CHECK(drawn.anythingChanged());
    CHECK(drawn.count > 0);

    // The background is still there everywhere the string is not, which is the
    // other half of the claim: the fill did not cover the window.
    CHECK(drawn.width() < 380);
    CHECK(drawn.height() < 190);
}

void testAnEmptyFontHandleIsAnError()
{
    // Same rule as `drawTexture`, and for the same reason. A string that silently
    // fails to appear is a bug that surfaces much later as missing text with
    // nothing pointing at the cause, so the mistake is reported where it was made.
    RealRenderFixture fixture;

    const Font empty;

    bool threw = false;
    std::string message;
    try
    {
        fixture.renderer().beginFrame();
        fixture.renderer().clear(engine::kBlack);
        fixture.renderer().drawText(empty, "X", 24U, engine::kWhite, RenderTransform{Vec2{200.0F, 100.0F}});
    }
    catch (const std::logic_error& error)
    {
        threw = true;
        message = error.what();
    }

    CHECK(threw);
    CHECK(has(message, "empty font handle"));
}

void testExistingRenderingStillWorks()
{
    // Text was appended, so the queries that existed before must be untouched. Both
    // paths, because a change to either would be a regression in the whole project
    // rather than in this phase: a rectangle through the real renderer, and the
    // three non-text queries through the recording one.
    RealRenderFixture fixture;

    fixture.renderer().beginFrame();
    fixture.renderer().clear(engine::kBlack);
    fixture.renderer().drawRectangle(Vec2{60.0F, 40.0F}, Color{1.0F, 0.0F, 0.0F, 1.0F},
                                     RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    sf::Texture readback;
    readback.create(fixture.window().getSize().x, fixture.window().getSize().y);
    readback.update(fixture.window());
    const sf::Image image = readback.copyToImage();

    const sf::Color centre = image.getPixel(100, 50);
    const sf::Color corner = image.getPixel(5, 5);
    fixture.renderer().endFrame();

    // The rectangle is still centred, still red, and the background is still there.
    CHECK(centre.r == 255);
    CHECK(centre.g == 0);
    CHECK(centre.b == 0);
    CHECK(corner.r == 0);
    CHECK(corner.g == 0);
    CHECK(corner.b == 0);

    // And drawing a string afterwards does not disturb it.
    DoubleAssetManager assets;
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager world;
    Entity& box = world.addEntity("box");
    box.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    box.addComponent<engine::components::Rectangle>();
    addTextEntity(world, "OVER", kFontPixeled, 24U);

    const ActionState actions;
    system.update(world, actions, 0.0F);

    CHECK(renderer.order().size() == 2U);
    CHECK(renderer.textDraws().size() == 1U);
}

void testTextGoesThroughTheRenderSystemToRealPixels()
{
    // The whole chain, end to end, with a real manager, a real renderer, a real
    // window and a real font: a Text component on an entity becomes pixels in the
    // right place. Every other group tests one link; this one tests the path.
    RealRenderFixture fixture;
    Camera camera = identityCamera();
    engine::systems::RenderSystem system{fixture.renderer(), camera, fixture.assets()};

    EntityManager world;
    addTextEntity(world, "END2END", kFontTech, 24U, Vec2{200.0F, 100.0F});

    const ActionState actions;

    fixture.renderer().beginFrame();
    fixture.renderer().clear(engine::kBlack);
    system.update(world, actions, 0.0F);

    const ChangedPixels drawn = measureChanged(fixture.window(), RealRenderFixture::background());

    fixture.renderer().endFrame();

    CHECK(drawn.anythingChanged());

    if (drawn.anythingChanged())
    {
        // Centred on the Transform the component's entity carried, through the
        // camera, exactly as the direct draw calls were.
        CHECK_NEAR(drawn.centreX(), 200.0F, 1.0F);
        CHECK_NEAR(drawn.centreY(), 100.0F, 1.0F);
    }
}

// ===========================================================================
// F. Source boundary
// ===========================================================================

void testTheTextComponentHeaderNamesNoGraphicsType()
{
    const std::string code = codeOf(ENGINE_TEXT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK_FALSE(has(code, "sf::"));
    CHECK_FALSE(has(code, "<SFML"));
    CHECK_FALSE(has(code, "SFML/"));
    CHECK_FALSE(has(code, "#include \"engine/graphics/"));
    CHECK_FALSE(has(code, "#include \"engine/assets/"));
}

void testTheTextComponentHeaderIncludesOnlyWhatItNeeds()
{
    // A whole-type check. The component needs a string and a fixed-width integer
    // and nothing else - not a colour, not a vector, not a transform - so the
    // includes are pinned rather than a forbidden list maintained by hand.
    const std::string code = codeOf(ENGINE_TEXT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(has(code, "#include <cstdint>"));
    CHECK(has(code, "#include <string>"));
    CHECK(countOf(code, "#include") == 2U);
}

void testOnlyTheSfmlImplementationNamesASfmlText()
{
    // `sf::Text` may be named in exactly one place: the SFML-backed renderer. This
    // is the whole "SFML stays behind the boundary" claim, stated as a scan over
    // the files this phase touched rather than as a rule in a comment.
    const std::string rendererSource = codeOf(ENGINE_SFML_RENDERER_SOURCE);

    CHECK(!rendererSource.empty());
    if (!rendererSource.empty())
    {
        // It is there, and it is used: this is the implementation, after all.
        CHECK(has(rendererSource, "sf::Text"));
        CHECK(has(rendererSource, "setCharacterSize"));
        CHECK(has(rendererSource, "setString"));
        CHECK(has(rendererSource, "getLocalBounds"));
    }

    // And nowhere else this phase touched.
    for (const char* path : {ENGINE_TEXT_HEADER, ENGINE_RENDERER_HEADER, ENGINE_RENDER_SYSTEM_SOURCE,
                             ENGINE_SFML_RENDERER_HEADER, ENGINE_FONT_HEADER})
    {
        const std::string code = codeOf(path);
        CHECK(!code.empty());
        if (code.empty())
        {
            continue;
        }

        if (has(code, "sf::Text"))
        {
            std::cerr << "    sf::Text leaked into " << path << '\n';
        }

        CHECK_FALSE(has(code, "sf::Text"));
    }
}

void testTheRenderSystemHeaderStaysSfmlFree()
{
    // The system names components and calls an interface. Neither needs a graphics
    // library, and a header that reached for one would put SFML into every
    // translation unit that names a system.
    const std::string code = codeOf(ENGINE_RENDER_SYSTEM_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK_FALSE(has(code, "sf::"));
    CHECK_FALSE(has(code, "<SFML"));
    CHECK_FALSE(has(code, "SFML/"));
}

void testTheFontHandleGainedOnlyTheRendererAsAFriend()
{
    // The native boundary was *extended*, not widened. `Font` now names the
    // renderer as a friend, which is the same second friend `Texture` has, so the
    // only two ways into a handle remain the loader that fills it and the renderer
    // that reads it. A third would be a new kind of access, not a new kind of
    // caller, and would show up here as an extra friend.
    const std::string code = codeOf(ENGINE_FONT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(has(code, "class SfmlAssetManager;"));
    CHECK(has(code, "friend class SfmlAssetManager;"));
    CHECK(has(code, "friend class graphics::SfmlRenderer;"));

    // Exactly two friends, so a new one cannot arrive unnoticed.
    CHECK(countOf(code, "friend class") == 2U);

    // And the forward declaration that makes the second name possible, which
    // `AssetHandleTest` separately requires the *header* to stay free of any
    // graphics type.
    CHECK(has(code, "class SfmlRenderer;"));
    CHECK_FALSE(has(code, "namespace sf"));
    CHECK_FALSE(has(code, "sf::Font"));
    CHECK_FALSE(has(code, "sf::Texture"));
}

void testTheFontImplementationPointerStaysPrivate()
{
    // Phase 14 widened the *friends* and had to prove it widened nothing else. A
    // friend is "this class may reach in"; the access specifier is "nobody else
    // may". Losing the second while keeping the first would leave a handle whose
    // native resource any caller could replace, and nothing would notice.
    //
    // Stated as a source check because "is this member accessible" is a question
    // the type system answers by refusing to compile, and a test cannot ask it
    // without trying to violate it. What is asserted is that the `private` label
    // still governs the section the member is declared in.
    const std::string code = codeOf(ENGINE_FONT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    const std::size_t privateAt = code.find("private:");
    const std::size_t implAt = code.find("struct Impl;");
    const std::size_t memberAt = code.find("m_impl;");

    CHECK(privateAt != std::string::npos);
    CHECK(implAt != std::string::npos);
    CHECK(memberAt != std::string::npos);

    if (privateAt == std::string::npos || implAt == std::string::npos || memberAt == std::string::npos)
    {
        return;
    }

    // The implementation type and its single member are both declared after the
    // `private` label and before the next access specifier or the class's end.
    CHECK(privateAt < implAt);
    CHECK(implAt < memberAt);

    // And `private:` is the **last** access specifier in the class, so there is no
    // second entrance into the handle after it.
    //
    // Not "there is no public section", which would be wrong: a class body opens
    // with `public:` for its constructors and move operations, and Font's does too.
    // The claim being made is narrower and is about order - everything after
    // `private:` stays inside it.
    // `rfind` returns `npos` for a specifier that is not there at all, and every
    // comparison against `npos` is false - so a missing `protected:` has to be
    // recognised as missing rather than compared. That trap is the reason the
    // check is written out rather than done with a clever expression.
    const std::size_t lastPublic = code.rfind("public:");
    if (lastPublic != std::string::npos)
    {
        CHECK(privateAt > lastPublic);
    }

    const std::size_t lastProtected = code.rfind("protected:");
    if (lastProtected != std::string::npos)
    {
        CHECK(privateAt > lastProtected);
    }
}

void testTheShippedGameAddsATextEntity()
{
    // The end-to-end demonstration, pinned at the only place it can be observed.
    //
    // The game builds its world in a function in `main`, which is not exposed as a
    // callable unit, so there is nothing for a test to invoke and assert on. The
    // game binary is already covered as a smoke test - there is a ctest that runs
    // `game --frames 5` - and that test is what catches a *broken* label, because a
    // missing font makes the game fail at run time. What it cannot catch is a label
    // that is quietly absent, because an absent label costs the game nothing.
    //
    // So this is a source check, and it is honest about being one. The proper fix is
    // for a later phase to give the world a name a test can call, which is scene
    // work; until then this says the demonstration exists rather than pretending a
    // runtime test could.
    const std::string code = codeOf(ENGINE_MAIN_SOURCE);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // It adds a label entity, from a helper, carrying a Text component.
    CHECK(has(code, "addTextLabel("));
    CHECK(has(code, "addComponent<Text>"));
    CHECK(has(code, "#include \"engine/components/Text.hpp\""));

    // And the label names a committed font rather than an invented one. The three
    // names are pinned so a typo in a level of indirection is caught here rather
    // than as a game that exits non-zero.
    CHECK(has(code, "\"fonts_pixeled\""));

    // Exactly one label, so "a demonstration" cannot quietly become "a pile of
    // them" without anyone noticing: one declaration and one call.
    CHECK(countOf(code, "addTextLabel(") == 2U);

    // And the call is a **statement**, not something sitting in a disabled branch.
    //
    // Counting occurrences is a weak proxy, and a mutation found that out: writing
    // `if (false) addTextLabel(...)` left the count at two and the label gone. An
    // absent label costs the game nothing, so nothing at run time notices - the
    // game would still exit zero and still draw a level. Requiring the call to
    // *start* its own line is what makes the check say something about the call
    // happening rather than about the text being present in the file.
    bool foundCallStatement = false;
    {
        std::istringstream lines{code};
        std::string line;
        while (std::getline(lines, line))
        {
            const std::size_t firstNonSpace = line.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos && line.compare(firstNonSpace, 13, "addTextLabel(") == 0)
            {
                foundCallStatement = true;
                break;
            }
        }
    }

    if (!foundCallStatement)
    {
        std::cerr << "    main adds a text label, but not as a statement it reaches\n";
    }

    CHECK(foundCallStatement);
}

void testTheFontHeaderStillIncludesOnlyMemory()
{
    // `AssetHandleTest` pins this, and it is worth checking here too: the friend
    // and the forward declaration must not have brought a header in with them.
    const std::string code = codeOf(ENGINE_FONT_HEADER);

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    CHECK(has(code, "#include <memory>"));
    CHECK(countOf(code, "#include") == 1U);
    CHECK_FALSE(has(code, "#include <SFML"));
    CHECK_FALSE(has(code, "#include \"SFML"));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        // A. The component
        {"text defaults", &testTextDefaults},
        {"text is an aggregate", &testTextIsAnAggregate},
        {"text is copyable and independent", &testTextIsCopyableAndIndependent},
        {"text is stored on entities", &testTextIsStoredOnEntities},
        {"text carries no colour and no anchor", &testTextCarriesNoColourAndNoAnchor},
        // B. The renderer API
        {"the renderer interface has a text draw", &testTheRendererInterfaceHasATextDraw},
        {"the renderer header is sfml free", &testTheRendererHeaderIsSfmlFree},
        {"the renderer header names no text object", &testTheRendererHeaderNamesNoTextObject},
        {"the existing renderer methods are unchanged", &testTheExistingRendererMethodsAreUnchanged},
        // C. Font lookup
        {"a missing font name still throws", &testAMissingFontNameStillThrows},
        {"the real manager resolves every committed font", &testTheRealManagerResolvesEveryCommittedFont},
        {"a real font handle can be resolved and drawn", &testARealFontHandleCanBeResolvedAndDrawn},
        // D. RenderSystem
        {"the text query draws entities with text", &testTheTextQueryDrawsEntitiesWithText},
        {"the text query resolves the font by name", &testTheTextQueryResolvesTheFontByName},
        {"the text query passes the component character size through",
         &testTheTextQueryPassesTheComponentCharacterSizeThrough},
        {"a missing font propagates out of the text query", &testAMissingFontPropagatesOutOfTheTextQuery},
        {"the text query reads the transform and never writes it", &testTheTextQueryReadsTheTransformAndNeverWritesIt},
        {"the text query applies the camera like every other query", &testTheTextQueryAppliesTheCameraLikeEveryOtherQuery},
        {"the text query draws last of all four queries", &testTheTextQueryDrawsLastOfAllFourQueries},
        {"a string is drawn alongside something else on the same entity",
         &testAStringIsDrawnAlongsideSomethingElseOnTheSameEntity},
        {"the text query ignores entities missing a component", &testTheTextQueryIgnoresEntitiesMissingAComponent},
        {"an empty string is submitted and the renderer decides what it means",
         &testAnEmptyStringIsSubmittedAndTheRendererDecidesWhatItMeans},
        // E. Real rendering
        {"text changes pixels", &testTextChangesPixels},
        {"text is centred on its position", &testTextIsCentredOnItsPosition},
        {"moving the position moves the text", &testMovingThePositionMovesTheText},
        {"character size changes how big the text is", &testCharacterSizeChangesHowBigTheTextIs},
        {"scale enlarges the text like any other renderable", &testScaleEnlargesTheTextLikeAnyOtherRenderable},
        {"the font actually changes the output", &testTheFontActuallyChangesTheOutput},
        {"the content changes the output", &testTheContentChangesTheOutput},
        {"the specific characters are rendered not just the length",
         &testTheSpecificCharactersAreRenderedNotJustTheLength},
        {"the colour is honoured", &testTheColourIsHonoured},
        {"an empty string draws nothing", &testAnEmptyStringDrawsNothing},
        {"text on a non black background is visible", &testTextOnANonBlackBackgroundIsVisible},
        {"an empty font handle is an error", &testAnEmptyFontHandleIsAnError},
        {"existing rendering still works", &testExistingRenderingStillWorks},
        {"text goes through the render system to real pixels", &testTextGoesThroughTheRenderSystemToRealPixels},
        // F. Source boundary
        {"the text component header names no graphics type", &testTheTextComponentHeaderNamesNoGraphicsType},
        {"the text component header includes only what it needs",
         &testTheTextComponentHeaderIncludesOnlyWhatItNeeds},
        {"only the sfml implementation names a sfml text", &testOnlyTheSfmlImplementationNamesASfmlText},
        {"the render system header stays sfml free", &testTheRenderSystemHeaderStaysSfmlFree},
        {"the font handle gained only the renderer as a friend", &testTheFontHandleGainedOnlyTheRendererAsAFriend},
        {"the font implementation pointer stays private", &testTheFontImplementationPointerStaysPrivate},
        {"the shipped game adds a text entity", &testTheShippedGameAddsATextEntity},
        {"the font header still includes only memory", &testTheFontHeaderStillIncludesOnlyMemory},
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

    std::cout << groupCount << " text rendering test groups passed\n";
    return EXIT_SUCCESS;
}
