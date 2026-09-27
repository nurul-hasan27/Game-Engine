#pragma once

#include "engine/assets/AssetManager.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Forward declared on purpose, exactly as graphics/SfmlRenderer.hpp does it: this
// header stays includable without a single SFML header. The one concrete type
// named here is the return type of a diagnostics accessor; every other graphics
// type lives entirely in the .cpp.
namespace sf
{
class Font;
class Texture;
} // namespace sf

namespace engine::assets
{

/// Thrown when a declared asset cannot be read from disk.
///
/// This is the third of three separate failure modes, and it is deliberately not
/// interchangeable with either of the others:
///
/// | Error | Means | Whose fault |
/// | ----- | ----- | ----------- |
/// | `AssetParseError` | the configuration file is malformed | whoever wrote the file |
/// | `AssetNotFoundError` | a name has no entry | whoever wrote the level or code |
/// | `AssetLoadError` | an entry names a file that will not load | the file, or the path |
///
/// Keeping them apart matters because the third is the only one that is usually a
/// deployment problem: a valid configuration, correct names, and a file that is
/// missing or corrupt on this machine. The message therefore always carries both
/// the asset name and the path that was actually tried, so the fix is obvious
/// without a debugger.
class AssetLoadError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// The concrete, SFML-backed [AssetManager](AssetManager.hpp).
///
/// ### Everything is loaded once, at construction
///
/// The constructor reads the configuration file, loads every asset it declares,
/// and throws if any of them fails. There is no lazy loading and there is no way
/// to add, replace or remove an asset afterwards.
///
/// That is not a simplification, it is what makes the interface safe. A manager
/// hands out **references** into a collection it owns, so the collection has to be
/// frozen for those references to mean anything. Loading on construction is what
/// guarantees a `const Texture&` a caller is holding stays valid, and a caller
/// cannot have to reason about whether some other call invalidated it.
///
/// ### Paths are resolved against the configuration file
///
/// A path in the file is read relative to the directory containing that file, not
/// relative to the process working directory. The working directory is wherever
/// the game happened to be launched from, which is not something a data file
/// should depend on. It also means an asset configuration can be moved, together
/// with the library it names, and keep working.
///
/// ### Why this is the one place SFML is allowed
///
/// Loading an image needs a graphics library. The engine has exactly two such
/// boundaries already — `SfmlKeyMap` for input and `SfmlRenderer` for drawing —
/// and this is the third, for assets. Everything above it works in handles, and
/// everything below the interface is free of the interface's own types.
///
/// ### Caching
///
/// Each declared asset is loaded exactly once and then returned by reference.
/// Repeated lookups of the same name return the *same object*, not a copy and not
/// a re-load. The map guarantees this: rehashes of an `unordered_map` invalidate
/// iterators but never references or pointers to its elements, so a reference
/// handed out during construction stays valid for the manager's whole lifetime.
class SfmlAssetManager final : public AssetManager
{
public:
    /// Reads `configurationPath`, then loads every asset it declares.
    ///
    /// Path resolution and the error modes are described on the class.
    ///
    /// @throws AssetParseError if the file is malformed, with the line number.
    ///         Propagated from the parser unchanged, because its diagnosis is
    ///         better than anything this constructor could add.
    /// @throws AssetLoadError if the file cannot be opened, or any declared
    ///         asset cannot be loaded. The message names the asset and the path.
    explicit SfmlAssetManager(const std::filesystem::path& configurationPath);

    /// Releases every loaded resource. The handles are copies owned by this
    /// manager, so a reference a caller still holds becomes invalid here, which
    /// is the same lifetime rule as any other owner.
    ~SfmlAssetManager() override = default;

    /// The texture declared under `name`, loaded at construction.
    ///
    /// @throws AssetNotFoundError if no texture is declared under that name.
    [[nodiscard]] const Texture& texture(std::string_view name) const override;

    /// The font declared under `name`, loaded at construction.
    ///
    /// @throws AssetNotFoundError if no font is declared under that name.
    [[nodiscard]] const Font& font(std::string_view name) const override;

    /// The platform texture behind `handle`, for verification and diagnostics.
    ///
    /// Drawing never goes through this: rendering goes through
    /// `graphics::Renderer` in a later phase. It exists because a handle
    /// deliberately exposes no way to read its own contents, which leaves no
    /// other way to check what was actually loaded - the size of a texture, or
    /// the pixel data of a palette-indexed PNG.
    ///
    /// @throws AssetLoadError if the handle is empty, which can only happen if
    ///         one was default constructed rather than obtained from a manager.
    [[nodiscard]] const sf::Texture& nativeTexture(const Texture& handle) const;

    /// The platform font behind `handle`, for verification and diagnostics.
    ///
    /// The font counterpart of `nativeTexture()`, and it exists for the same
    /// reason: a handle exposes no way to read its contents, so without this
    /// there is no way to tell a font that parsed from a file that merely exists
    /// and happens to be a valid file. The family name is observable here and is
    /// empty for anything that did not parse.
    ///
    /// @throws AssetLoadError if the handle is empty.
    [[nodiscard]] const sf::Font& nativeFont(const Font& handle) const;

    /// Number of textures loaded. Diagnostics only.
    [[nodiscard]] std::size_t textureCount() const noexcept { return m_textures.size(); }

    /// Number of fonts loaded. Diagnostics only.
    [[nodiscard]] std::size_t fontCount() const noexcept { return m_fonts.size(); }

private:
    /// Resolves `path` as written in the configuration against the directory the
    /// configuration lives in.
    [[nodiscard]] std::filesystem::path resolve(const std::string& path) const;

    std::filesystem::path m_configurationPath;
    std::unordered_map<std::string, Texture> m_textures;
    std::unordered_map<std::string, Font> m_fonts;
};

} // namespace engine::assets
