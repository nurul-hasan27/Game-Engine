#include "engine/assets/SfmlAssetManager.hpp"

#include "engine/assets/AssetFile.hpp"

// The concrete graphics types, and the definitions of the handles' private
// implementations. This is the only file in the asset system that needs them.
#include "AssetHandleNative.hpp"

#include <SFML/Graphics/Font.hpp>
#include <SFML/Graphics/Texture.hpp>

#include <fstream>
#include <istream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets
{
namespace
{

/// Reads a whole file. Returns false rather than throwing, so the caller can
/// produce a message that names the file it was asked for.
[[nodiscard]] bool readFile(const std::filesystem::path& path, std::string& contents)
{
    std::ifstream file{path};
    if (!file)
    {
        return false;
    }

    contents.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    return true;
}

} // namespace

std::filesystem::path SfmlAssetManager::resolve(const std::string& path) const
{
    // relative() against the configuration's own directory, never the process
    // working directory. An asset file that depended on where the game was
    // launched from would be one CWD change away from silently loading nothing.
    return m_configurationPath.parent_path() / path;
}

SfmlAssetManager::SfmlAssetManager(const std::filesystem::path& configurationPath)
    : m_configurationPath{configurationPath}
{
    std::string contents;
    if (!readFile(configurationPath, contents))
    {
        throw AssetLoadError{"cannot open asset configuration '" + configurationPath.string() + "'"};
    }

    // Malformed lines and duplicate names are parser problems, and the parser's
    // messages carry a line number. They are deliberately not caught and
    // rewrapped: there is nothing this constructor could add, and flattening
    // them into a load error would throw away the only useful part.
    const std::vector<AssetEntry> entries = parseAssetFile(contents);

    for (const AssetEntry& entry : entries)
    {
        const std::filesystem::path resolved = resolve(entry.path);

        if (entry.type == AssetType::Texture)
        {
            sf::Texture native;
            if (!native.loadFromFile(resolved.string()))
            {
                throw AssetLoadError{"cannot load texture '" + entry.name + "' from '" + resolved.string() + "'"};
            }

            // loadFromFile is enough for a palette-indexed PNG. stb_image, which
            // SFML uses, is asked for four channels and resolves the palette
            // itself, so the pixels are correct without a detour through
            // copyToImage or loadFromImage. That was verified pixel by pixel
            // against an independent decode of explosion.png, Flagpole.png and
            // question.png, all three of which are indexed. If a future graphics
            // library ever changes that, the loader tests fail on the pixel
            // comparison rather than on somebody noticing wrong colours on screen.
            //
            // Smooth filtering is deliberately left at the library default, which
            // is nearest-neighbour. No smoothing API is called anywhere in the
            // engine.
            //
            // Recorded as a decision rather than an oversight, because the two
            // sources genuinely disagree: the assignment specification does not
            // mention smoothing at all, while the course's reference
            // implementation defaults its texture loader to smooth = true. Where a
            // specification is silent and a reference implementation guesses, the
            // specification wins - adopting the guess would be introducing a
            // requirement nobody wrote down.
            //
            // Nearest-neighbour is also the right choice for this artwork, which
            // is hand-placed 2D pixel art with a deliberate limited palette, so
            // smoothing would soften edges the art depends on.
            //
            // Changing this is not a one-line edit: it changes rendered output, so
            // it needs a documented decision and a pixel test to match.
            const auto inserted = m_textures.try_emplace(entry.name);
            inserted.first->second.m_impl = std::make_unique<Texture::Impl>();
            inserted.first->second.m_impl->native = std::move(native);
        }
        else
        {
            sf::Font native;
            if (!native.loadFromFile(resolved.string()))
            {
                throw AssetLoadError{"cannot load font '" + entry.name + "' from '" + resolved.string() + "'"};
            }

            // Loaded, validated and cached. Nothing draws a font in this phase,
            // and no measurement or glyph API is added, because nothing needs one
            // yet and a text interface designed without a consumer would be
            // redesigned the moment one appeared.
            const auto inserted = m_fonts.try_emplace(entry.name);
            inserted.first->second.m_impl = std::make_unique<Font::Impl>();
            inserted.first->second.m_impl->native = std::move(native);
        }
    }
}

const Texture& SfmlAssetManager::texture(const std::string_view name) const
{
    const auto found = m_textures.find(std::string{name});
    if (found == m_textures.end())
    {
        throw AssetNotFoundError{"no texture named '" + std::string{name} + "'"};
    }

    return found->second;
}

const Font& SfmlAssetManager::font(const std::string_view name) const
{
    const auto found = m_fonts.find(std::string{name});
    if (found == m_fonts.end())
    {
        throw AssetNotFoundError{"no font named '" + std::string{name} + "'"};
    }

    return found->second;
}

const sf::Texture& SfmlAssetManager::nativeTexture(const Texture& handle) const
{
    // Read through the private implementation, which only this class is a friend
    // of. An empty handle has no resource, and handing back one silently would
    // turn a default-constructed handle into a crash somewhere else entirely.
    if (handle.m_impl == nullptr)
    {
        throw AssetLoadError{"the texture handle is empty, so it has no platform texture"};
    }

    return handle.m_impl->native;
}

const sf::Font& SfmlAssetManager::nativeFont(const Font& handle) const
{
    if (handle.m_impl == nullptr)
    {
        throw AssetLoadError{"the font handle is empty, so it has no platform font"};
    }

    return handle.m_impl->native;
}

} // namespace engine::assets
