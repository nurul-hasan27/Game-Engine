#pragma once

#include "engine/assets/Font.hpp"
#include "engine/assets/Texture.hpp"

#include <stdexcept>
#include <string_view>

namespace engine::assets
{

/// Thrown when a lookup names an asset the manager does not hold.
///
/// Derives from `std::out_of_range`, which is what a missing key means and
/// exactly what `std::map::at` throws, so a caller that already handles
/// map-style lookups keeps working unchanged.
///
/// It is deliberately a **different** type from
/// [AssetParseError](AssetFile.hpp). The two failures are diagnosed in different
/// places and need different fixes: a parse error means the configuration file is
/// wrong and belongs to whoever wrote it, while a lookup failure means a name in
/// a level file or a piece of code has no matching entry. Telling them apart
/// lets a tool report "your asset file is malformed" separately from "this entity
/// refers to an asset you never declared".
class AssetNotFoundError : public std::out_of_range
{
public:
    using std::out_of_range::out_of_range;
};

/// Read-only access to the assets declared by a configuration file.
///
/// ### Why an interface at all
///
/// The engine already separates every other external dependency the same way:
/// `Input` from `SfmlKeyMap`, `Renderer` from `SfmlRenderer`. This continues that
/// pattern, and for the same reason. The *policy* — which name maps to which
/// resource, and what happens when a name is unknown — is ordinary engine logic
/// that deserves to be testable and readable on its own. Only the *loading* of a
/// file needs SFML. Putting the two apart means the policy can be tested with no
/// window, no GPU and no files, and it means a test can substitute its own
/// manager without loading anything.
///
/// The concrete implementation, which does the loading, comes later. Nothing in
/// this header names it, so nothing above it has to know which one it has.
///
/// ### The interface cannot change what is loaded
///
/// There is no `load`, no `clear`, and no way to add or replace an entry. This is
/// the point of the type, not an omission.
///
/// A manager hands out **references** to resources it owns. If it also offered a
/// way to reload, then a `const Texture&` a caller had taken a moment earlier
/// could be left dangling by a call it had no reason to believe was mutating —
/// and the caller would have no way to write safe code, because the interface
/// would not have told it the danger existed. Freezing the collection at
/// construction time is what makes a reference safe to hold.
///
/// Loading therefore happens by constructing a manager, not by calling a method
/// on one. Anything that outlives a reload gets a new manager, and the old
/// references stay valid for as long as the old manager does.
///
/// ### Lookups return references, and throw when absent
///
/// `texture()` returns `const Texture&`, not a copy and not a pointer. A handle
/// owns a resource and is non-copyable, so a copy is not even available; and a
/// pointer would push a null check onto every call site and make the ordinary
/// case two tokens longer for no gain. Returning a reference puts "this must
/// exist" in the type instead of in a comment.
///
/// A name that is not declared throws [AssetNotFoundError](AssetNotFoundError)
/// rather than returning something empty. A blank sprite with no explanation is
/// a far worse failure than a loud one, and an asset name that matches nothing is
/// a mistake in a level file or in code, not a normal runtime state.
///
/// There is deliberately no non-throwing `hasTexture` yet. One will be added the
/// first time something actually needs to branch on absence, and until then a
/// caller that wants to check has to decide what catching the exception means
/// for it.
class AssetManager
{
public:
    /// Allows a manager to be deleted through this interface.
    ///
    /// The concrete implementation owns real resources, so it must be
    /// destructible through a base pointer.
    virtual ~AssetManager() = default;

    /// Assets are a shared, shared-ownership-free collection, so a manager is
    /// never copied or moved. It is held by reference, like `Renderer`.
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;

    /// The texture declared under `name`.
    ///
    /// The returned reference is owned by the manager, stays valid for as long as
    /// the manager does, and is the same object on every call with the same name.
    /// Callers should hold the reference, not a copy: handles are non-copyable,
    /// and copying one is not a thing this type permits.
    ///
    /// @param name The asset name from the configuration file. Compared exactly,
    ///        so case and spacing matter.
    /// @throws AssetNotFoundError if no texture is declared under that name.
    [[nodiscard]] virtual const Texture& texture(std::string_view name) const = 0;

    /// The font declared under `name`.
    ///
    /// The same guarantees as [texture()](AssetManager.hpp): a stable reference
    /// owned by the manager, the same object on every call with the same name.
    ///
    /// Text rendering is out of scope for now. This exists so a font can be
    /// loaded, validated and cached, which is a complete and testable
    /// responsibility, and no more.
    ///
    /// @param name The asset name from the configuration file. Compared exactly.
    /// @throws AssetNotFoundError if no font is declared under that name.
    [[nodiscard]] virtual const Font& font(std::string_view name) const = 0;

protected:
    /// Managers are created by their concrete implementation and held by
    /// reference, so the constructor is not part of the public surface. Copy and
    /// move are deleted above; sealing construction stops a manager being
    /// sliced into this abstract type by accident.
    AssetManager() = default;
};

} // namespace engine::assets
